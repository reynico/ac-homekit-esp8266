#include <Arduino.h>
#include <arduino_homekit_server.h>
#include "wifi_info.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Whirlpool.h>
#include <DHT.h>

#define LOG_D(fmt, ...) printf_P(PSTR(fmt "\n"), ##__VA_ARGS__);

const uint16_t kIrLed = 15; // D4
IRWhirlpoolAc ac(kIrLed);

int powerStatus = 2;

DHT dht (5, DHT22);

// Globals
bool queueCommand = false;
bool off = true;
void flipQueueCommand(bool newState) {
  Serial.write("Flipping queueCommand to %d\n", newState);
  queueCommand = newState;
}

void setup() {
  Serial.begin(115200);
  pinMode(powerStatus, INPUT);
  digitalWrite(powerStatus, HIGH);
  dht.begin();
  wifi_connect();
  // homekit_storage_reset(); // to remove the previous HomeKit pairing storage when you first run this new HomeKit example
  my_homekit_setup();
  Serial.write("HomeKit setup complete. About to start ac.begin()\n");
  ac.begin();
  ac.setPowerToggle(false);
}

void updatePowerStatus() {
  bool isOn = true;
  isOn = digitalRead(powerStatus);
  if (isOn) {
    off = false;
  } else {
    off = true;
    ac.setPowerToggle(true);
    ac.send();
    ac.setPowerToggle(false);
  }
  LOG_D("AC power is: %s", isOn ? "ON" : "OFF");
}

void loop() {
  my_homekit_loop();
  delay(10);

  if (queueCommand) {
    if (off) {
      LOG_D("AC is OFF, and needs to be turned on. Set setPowerToggle to true")
      // ac.setPowerToggle(true);
      // off = false;
    }
    Serial.write("Sending AC Command...\n");
    ac.send();
    flipQueueCommand(false);
    // ac.setPowerToggle(false);
    // LOG_D("AC is now ON, set setPowerToggle to false");
  }
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
  cooler_active.value = value;

  LOG_D("ACTIVE was %s and is now set to %s", oldState ? "ON" : "OFF", state ? "ON" : "OFF");

  if (!state && oldState) {
    LOG_D("As the AC is not active anymore, set off to true.")
    // off = true;
  }
  flipQueueCommand(true);
}

void report() {
  float temperature_value = dht.readTemperature();
	current_temp.value.float_value = temperature_value;
	LOG_D("Current temperature: %.1f", temperature_value);
	homekit_characteristic_notify(&current_temp, current_temp.value);
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
        // off = true;
        LOG_D("target_state_setter: OFF");
      break;
    case 1073646594:
      ac.setMode(kWhirlpoolAcCool);
      LOG_D("target_state_setter: Cool");
      break;
    case 1073646593:
      ac.setMode(kWhirlpoolAcHeat);
      LOG_D("target_state_setter: Heat");
      break;
    case 1073646592:
      ac.setMode(kWhirlpoolAcAuto);
      LOG_D("target_state_setter: Auto");
      break;
  }
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

  int fanSpeed = 0;  // fan mode disabled
  if (newSpeed < 33) {
    fanSpeed = 1;
  } else if (newSpeed < 66) {
    fanSpeed = 2;
  } else {
    fanSpeed = 3;
  }

  // Keep the fan always running
  if (fanSpeed == 0) {
    fanSpeed = 1;
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

void my_homekit_setup() {
  Serial.write("starting my_homekit_setup\n");
  //Add the .setter function to get the switch-event sent from iOS Home APP.
  //The .setter should be added before arduino_homekit_setup.
  //HomeKit sever uses the .setter_ex internally, see homekit_accessories_init function.
  //Maybe this is a legacy design issue in the original esp-homekit library,
  //and I have no reason to modify this "feature".

  cooler_active.setter = cooler_active_setter;
  current_state.setter = current_state_setter;
  target_state.setter = target_state_setter;
  rotation_speed.setter = rotation_speed_setter;
  cooling_threshold.setter = cooling_threshold_setter;
  heating_threshold.setter = cooling_threshold_setter;

  Serial.write("about to call arduino_homekit_setup\n");
  arduino_homekit_setup(&config);

  Serial.write("exiting my_homekit_setup\n");
}

void my_homekit_loop() {
  arduino_homekit_loop();
  const uint32_t t = millis();
  if (t > next_heap_millis) {
    // show heap info every 5 seconds
    next_heap_millis = t + 5 * 1000;

    // check the power status readings every 5 seconds
    updatePowerStatus();
    // LOG_D("Free heap: %d, HomeKit clients: %d",
    //       ESP.getFreeHeap(), arduino_homekit_connected_clients_count());
  }
  if (t > next_report_millis) {
		// report sensor values every 10 seconds
		next_report_millis = t + 10 * 1000;
		report();
	}
}
