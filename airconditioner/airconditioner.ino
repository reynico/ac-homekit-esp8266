#include <Arduino.h>
#include <arduino_homekit_server.h>
#include "wifi_info.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Whirlpool.h>
#include <DHT.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

#define LOG_D(fmt, ...) printf_P(PSTR(fmt "\n"), ##__VA_ARGS__);

const uint16_t ir_led_pin = 15;  // Wemos D1, pin D8
IRWhirlpoolAc ac(ir_led_pin);

int power_status_pin = 14;  // Wemos D1, pin D5
int reset_pin = 4;         // D7

const char* prometheus_server_address = "http://monitoring.home:9091/metrics/job/temperature/room/living_room";

DHT dht(5, DHT22);

// Globals
bool queue_command = false;
bool power_current_status = false;  // true when the AC is on
bool power_desired_status = false;
bool power_current_status_notified = false;
bool off = true;
String ac_mode = "0";

void setup() {
  Serial.begin(115200);
  pinMode(power_status_pin, INPUT);
  pinMode(reset_pin, INPUT_PULLUP);
  dht.begin();
  wifi_connect();
  if (!digitalRead(reset_pin)) {
    LOG_D("Reset button is ON, resetting status");
    homekit_storage_reset();
    LOG_D("Reset done, continue");
  }
  my_homekit_setup();
  Serial.write("HomeKit setup complete. About to start ac.begin()\n");
  ac.begin();
}

void flip_queue_command(bool new_state) {
  LOG_D("Flipping queueCommand to %d\n", new_state);
  queue_command = new_state;
}


void set_power_current_status() {
  int readings[3];

  for (int i = 0; i < 3; i++) {
    readings[i] = !digitalRead(power_status_pin);
    delay(10);  // Add a small delay to stabilize readings
  }

  if (readings[0] + readings[1] + readings[2] >= 2) {
    power_current_status = true;
  } else {
    power_current_status = false;
  }

  LOG_D("AC power current status: %s", power_current_status ? "ON" : "OFF");
}

void update_power_status() {
  LOG_D("AC power desired status: %s", power_desired_status ? "ON" : "OFF");
  if (power_current_status != power_desired_status) {
    LOG_D("Toggling power");
    ac.setPowerToggle(true);
    ac.send();
  }
  ac.setPowerToggle(false);
  set_power_current_status();
}

void loop() {
  my_homekit_loop();

  if (queue_command) {
    LOG_D("Sending AC Command...");
    ac.send();
    flip_queue_command(false);
  }

  delay(10);
}

// defined in my_accessory.c
extern "C" homekit_server_config_t config;
extern "C" homekit_characteristic_t cooler_active;
extern "C" homekit_characteristic_t current_temp;
extern "C" homekit_characteristic_t current_state;
extern "C" homekit_characteristic_t target_state;
extern "C" homekit_characteristic_t rotation_speed;
extern "C" homekit_characteristic_t cooling_threshold;
extern "C" homekit_characteristic_t heating_threshold;

static uint32_t next_heap_millis = 0;
static uint32_t next_report_millis = 0;

void cooler_active_setter(const homekit_value_t value) {
  bool old_state = cooler_active.value.bool_value;
  bool state = value.bool_value;
  set_power_current_status();
  cooler_active.value = value;

  LOG_D("Cooler active setter was %s and is now set to %s. The current power status is %s.", old_state ? "ON" : "OFF", state ? "ON" : "OFF", power_current_status ? "ON" : "OFF");

  if (old_state != power_current_status) {
    LOG_D("There is a mismatch between the oldState: %s and the power current status: %s. Fixing", old_state ? "ON" : "OFF", power_current_status ? "ON" : "OFF");
    old_state = power_current_status;
  }

  if (old_state && !state) {
    // AC is currently on, but the state asks to power it off
    power_desired_status = false;
    update_power_status();
    flip_queue_command(true);
  } else if (!old_state && state) {
    // AC is currently off, but the state asks to power it on
    power_desired_status = true;
    update_power_status();
    flip_queue_command(true);
  } else if (!old_state && !state) {
    // AC is currently off, and should be kept off
  }
}

void current_state_setter(const homekit_value_t value) {
  target_state.value = value;
  LOG_D("NO_OP: current_state_setter. Got value %d", value.int_value);
}

void target_state_setter(const homekit_value_t value) {
  int old_state = target_state.value.int_value;
  int state = value.int_value;
  target_state.value = value;

  if (old_state == state) {
    LOG_D("NO_OP: target_state_setter. Got value %d", value.int_value);
    return;
  }

  switch (value.int_value) {
    case 0:
      LOG_D("AC is NOT OFF, but should be OFF. Setting it to OFF")
      power_desired_status = false;
      ac_mode = "0";
      LOG_D("target_state_setter: OFF");
      break;
    case 1073646594:
      power_desired_status = true;
      ac_mode = "1";
      ac.setMode(kWhirlpoolAcCool);
      LOG_D("target_state_setter: Cool");
      break;
    case 1073646593:
      power_desired_status = true;
      ac_mode = "2";
      ac.setMode(kWhirlpoolAcHeat);
      LOG_D("target_state_setter: Heat");
      break;
    case 1073646592:
      power_desired_status = true;
      ac_mode = "3";
      ac.setMode(kWhirlpoolAcAuto);
      LOG_D("target_state_setter: Auto");
      break;
  }
  update_power_status();
  flip_queue_command(true);
}

void rotation_speed_setter(const homekit_value_t value) {
  float old_speed = rotation_speed.value.float_value;
  float new_speed = value.float_value;
  rotation_speed.value = value;  // sync the value

  LOG_D("ROTATION SPEED was %.2f and is now set to %.2f", old_speed, new_speed);

  if (old_speed == new_speed) {
    LOG_D("Rotation speed NO-OP");
    return;
  }

  int fan_speed = 0;  // fan mode auto
  if (new_speed < 33) {
    fan_speed = 3;  // turns out that the speed index is inverted, 3 is min, 1 is max.
  } else if (new_speed < 66) {
    fan_speed = 2;
  } else {
    fan_speed = 1;
  }

  // When in automatatic, keep the fan in low speed
  if (fan_speed == 0) {
    fan_speed = 3;
  }

  LOG_D("ROTATION speed AC value is %d", fan_speed);
  ac.setFan(fan_speed);
  flip_queue_command(true);
}

void cooling_threshold_setter(const homekit_value_t value) {
  float oldTemp = cooling_threshold.value.float_value;
  float temp = value.float_value;
  cooling_threshold.value = value;

  LOG_D("COOLER THRESHOLD was %.2f and is now set to %.2f", oldTemp, temp);

  ac.setTemp(temp);
  flip_queue_command(true);
}

void heating_threshold_setter(const homekit_value_t value) {
  float oldTemp = heating_threshold.value.float_value;
  float temp = value.float_value;
  heating_threshold.value = value;

  LOG_D("COOLER THRESHOLD was %.2f and is now set to %.2f", oldTemp, temp);

  ac.setTemp(temp);
  flip_queue_command(true);
}

void my_homekit_setup() {
  LOG_D("starting my_homekit_setup\n");

  cooler_active.setter = cooler_active_setter;
  current_state.setter = current_state_setter;
  target_state.setter = target_state_setter;
  rotation_speed.setter = rotation_speed_setter;
  cooling_threshold.setter = cooling_threshold_setter;
  heating_threshold.setter = heating_threshold_setter;

  LOG_D("about to call arduino_homekit_setup\n");
  arduino_homekit_setup(&config);

  LOG_D("exiting my_homekit_setup\n");
}

void my_homekit_loop() {
  arduino_homekit_loop();
  const uint32_t t = millis();
  if (t > next_heap_millis) {
    next_heap_millis = t + 1 * 5000;
    set_power_current_status();
    LOG_D("AC power is currently: %s", power_current_status ? "ON" : "OFF");
    if (power_current_status_notified != power_current_status) {
      LOG_D("Notify HomeKit that the AC is currently: %s", power_current_status ? "ON" : "OFF");
      cooler_active.value.bool_value = power_current_status;
      homekit_characteristic_notify(&cooler_active, cooler_active.value);
      power_current_status_notified = power_current_status;
    }
    // LOG_D("Free heap: %d, HomeKit clients: %d",
    //       ESP.getFreeHeap(), arduino_homekit_connected_clients_count());
  }
  if (t > next_report_millis) {
    // report sensor values every 10 seconds
    next_report_millis = t + 10 * 1000;
    report();
  }
}

void report() {
  float temperature_value = dht.readTemperature();
  float humidity_value = dht.readHumidity();
  if (isnan(temperature_value)) {
    LOG_D("Error while reading DHT temperature and humidity, defaulting to 20.0");
    temperature_value = 20.0;
    humidity_value = 20.0;
  }
  current_temp.value.float_value = temperature_value;
  LOG_D("Current temperature: %.1f", temperature_value);
  homekit_characteristic_notify(&current_temp, current_temp.value);
  prometheus_report(temperature_value, humidity_value);
}


void prometheus_report(float temperature, float humidity) {
  String ac_on = power_current_status ? "1" : "0";
  String data = "temperature_c " + String(temperature) + "\n";
  data += "humidity_percent " + String(humidity) + "\n";
  data += "ac_on " + String(ac_on) + "\n";
  data += "ac_mode " + String(ac_mode) + "\n";
  WiFiClient client;
  HTTPClient http;
  http.begin(client, prometheus_server_address);
  int http_response_code = http.POST(data);
  LOG_D("Prometheus HTTP Response code: %d", http_response_code);
  http.end();
}
