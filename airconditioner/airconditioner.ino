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

const uint16_t kIrLed = 15;  // D4
IRWhirlpoolAc ac(kIrLed);

int powerStatusPin = 12;  // D9
int resetPin = 4;         // D7
bool powerDesiredStatus = false;
bool powerCurrentStatusNotified = false;

const char* prometheusServerAddress = "http://monitoring.home:9091/metrics/job/temperature/room/living_room";

DHT dht(5, DHT22);

// Globals
bool queueCommand = false;
bool powerCurrentStatus = false;  // true when the AC is on
bool off = true;

void setup() {
  Serial.begin(115200);
  pinMode(powerStatusPin, INPUT);
  pinMode(resetPin, INPUT_PULLUP);
  dht.begin();
  wifi_connect();
  if (!digitalRead(resetPin)) {
    LOG_D("Reset button is ON, resetting status");
    homekit_storage_reset();
    LOG_D("Reset done, continue");
  }
  my_homekit_setup();
  Serial.write("HomeKit setup complete. About to start ac.begin()\n");
  ac.begin();
}

void flipQueueCommand(bool newState) {
  LOG_D("Flipping queueCommand to %d\n", newState);
  queueCommand = newState;
}


void setPowerCurrentStatus() {
  int readings[3];

  for (int i = 0; i < 3; i++) {
    readings[i] = !digitalRead(powerStatusPin);
    delay(10);  // Add a small delay to stabilize readings
  }

  if (readings[0] + readings[1] + readings[2] >= 2) {
    powerCurrentStatus = true;
  } else {
    powerCurrentStatus = false;
  }

  LOG_D("AC power current status: %s", powerCurrentStatus ? "ON" : "OFF");
}

void updatePowerStatus() {
  LOG_D("AC power desired status: %s", powerDesiredStatus ? "ON" : "OFF");
  if (powerCurrentStatus != powerDesiredStatus) {
    LOG_D("Toggling power");
    ac.setPowerToggle(true);
    ac.send();
  }
  ac.setPowerToggle(false);
  setPowerCurrentStatus();
}

void loop() {
  my_homekit_loop();

  if (queueCommand) {
    LOG_D("Sending AC Command...");
    ac.send();
    flipQueueCommand(false);
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
  bool oldState = cooler_active.value.bool_value;
  bool state = value.bool_value;
  setPowerCurrentStatus();
  cooler_active.value = value;

  LOG_D("Cooler active setter was %s and is now set to %s. The current power status is %s.", oldState ? "ON" : "OFF", state ? "ON" : "OFF", powerCurrentStatus ? "ON" : "OFF");

  if (oldState != powerCurrentStatus) {
    LOG_D("There is a mismatch between the oldState: %s and the power current status: %s. Fixing", oldState ? "ON" : "OFF", powerCurrentStatus ? "ON" : "OFF");
    oldState = powerCurrentStatus;
  }

  if (oldState && !state) {
    // AC is currently on, but the state asks to power it off
    powerDesiredStatus = false;
    updatePowerStatus();
    flipQueueCommand(true);
  } else if (!oldState && state) {
    // AC is currently off, but the state asks to power it on
    powerDesiredStatus = true;
    updatePowerStatus();
    flipQueueCommand(true);
  } else if (!oldState && !state) {
    // AC is currently off, and should be kept off
  }
}

void current_state_setter(const homekit_value_t value) {
  target_state.value = value;
  LOG_D("NO_OP: current_state_setter. Got value %d", value.int_value);
}

void target_state_setter(const homekit_value_t value) {
  int oldState = target_state.value.int_value;
  int state = value.int_value;
  target_state.value = value;

  if (oldState == state) {
    LOG_D("NO_OP: target_state_setter. Got value %d", value.int_value);
    return;
  }

  switch (value.int_value) {
    case 0:
      LOG_D("AC is NOT OFF, but should be OFF. Setting it to OFF")
      powerDesiredStatus = false;
      LOG_D("target_state_setter: OFF");
      break;
    case 1073646594:
      powerDesiredStatus = true;
      ac.setMode(kWhirlpoolAcCool);
      LOG_D("target_state_setter: Cool");
      break;
    case 1073646593:
      powerDesiredStatus = true;
      ac.setMode(kWhirlpoolAcHeat);
      LOG_D("target_state_setter: Heat");
      break;
    case 1073646592:
      powerDesiredStatus = true;
      ac.setMode(kWhirlpoolAcAuto);
      LOG_D("target_state_setter: Auto");
      break;
  }
  updatePowerStatus();
  flipQueueCommand(true);
}

void rotation_speed_setter(const homekit_value_t value) {
  float oldSpeed = rotation_speed.value.float_value;
  float newSpeed = value.float_value;
  rotation_speed.value = value;  //sync the value

  LOG_D("ROTATION SPEED was %.2f and is now set to %.2f", oldSpeed, newSpeed);

  if (oldSpeed == newSpeed) {
    LOG_D("Rotation speed NO-OP");
    return;
  }

  int fanSpeed = 0;  // fan mode auto
  if (newSpeed < 33) {
    fanSpeed = 3; // turns out that the speed index is inverted, 3 is min, 1 is max.
  } else if (newSpeed < 66) {
    fanSpeed = 2;
  } else {
    fanSpeed = 1;
  }

  // When in automatatic, keep the fan in low speed
  if (fanSpeed == 0) {
    fanSpeed = 3;
  }

  LOG_D("ROTATION speed AC value is %d", fanSpeed);
  ac.setFan(fanSpeed);
  flipQueueCommand(true);
}

void cooling_threshold_setter(const homekit_value_t value) {
  float oldTemp = cooling_threshold.value.float_value;
  float temp = value.float_value;
  cooling_threshold.value = value;

  LOG_D("COOLER THRESHOLD was %.2f and is now set to %.2f", oldTemp, temp);

  ac.setTemp(temp);
  flipQueueCommand(true);
}

void heating_threshold_setter(const homekit_value_t value) {
  float oldTemp = heating_threshold.value.float_value;
  float temp = value.float_value;
  heating_threshold.value = value;

  LOG_D("COOLER THRESHOLD was %.2f and is now set to %.2f", oldTemp, temp);

  ac.setTemp(temp);
  flipQueueCommand(true);
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
    setPowerCurrentStatus();
    LOG_D("AC power is currently: %s", powerCurrentStatus ? "ON" : "OFF");
    if (powerCurrentStatusNotified != powerCurrentStatus) {
      LOG_D("Notify HomeKit that the AC is currently: %s", powerCurrentStatus ? "ON" : "OFF");
      cooler_active.value.bool_value = powerCurrentStatus;
      homekit_characteristic_notify(&cooler_active, cooler_active.value);
      powerCurrentStatusNotified = powerCurrentStatus;
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
  String data = "temperature_c " + String(temperature) + "\n";
  data += "humidity_percent " + String(humidity) + "\n";
  WiFiClient client;
  HTTPClient http;
  http.begin(client, prometheusServerAddress);
  int httpResponseCode = http.POST(data);
  LOG_D("Prometheus HTTP Response code: %d", httpResponseCode);
  http.end();
}
