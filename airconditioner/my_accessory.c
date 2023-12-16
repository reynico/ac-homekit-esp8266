#include <homekit/homekit.h>
#include <homekit/characteristics.h>

void my_accessory_identify(homekit_value_t _value) {
  printf("accessory identify\n");
}

// 0:Inactive, 1:Active
homekit_characteristic_t cooler_active = HOMEKIT_CHARACTERISTIC_(ACTIVE, 0);

// float; min 0, max 100, step 0.1, unit celsius
homekit_characteristic_t current_temp = HOMEKIT_CHARACTERISTIC_(CURRENT_TEMPERATURE, 96);

homekit_characteristic_t current_state = HOMEKIT_CHARACTERISTIC_(CURRENT_HEATER_COOLER_STATE, 0);

// 0: auto, 1: heat, 2:cool
homekit_characteristic_t target_state = HOMEKIT_CHARACTERISTIC_(TARGET_HEATER_COOLER_STATE, 1,
  .valid_values={
    .count=2,
    .values=(uint8_t[]) {1, 2}
  },
);

// float 0-100
homekit_characteristic_t rotation_speed = HOMEKIT_CHARACTERISTIC_(ROTATION_SPEED, 0);

homekit_characteristic_t heating_threshold = HOMEKIT_CHARACTERISTIC_(HEATING_THRESHOLD_TEMPERATURE, 22,
  .min_value = (float[]) {17},
  .max_value = (float[]) {30},
  .min_step = (float[]) {1.0}
);

homekit_characteristic_t cooling_threshold = HOMEKIT_CHARACTERISTIC_(COOLING_THRESHOLD_TEMPERATURE, 22,
  .min_value = (float[]) {17},
  .max_value = (float[]) {30},
  .min_step = (float[]) {1.0}
);


homekit_accessory_t *accessories[] = {
  HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_air_conditioner, .services=(homekit_service_t*[]) {
    HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]) {
      HOMEKIT_CHARACTERISTIC(NAME, "Air Conditioner"),
      HOMEKIT_CHARACTERISTIC(MANUFACTURER, "LG"),
      HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "0000001"),
      HOMEKIT_CHARACTERISTIC(MODEL, "ESP8266"),
      HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "1.0"),
      HOMEKIT_CHARACTERISTIC(IDENTIFY, my_accessory_identify),
      NULL
    }),
    HOMEKIT_SERVICE(HEATER_COOLER, .primary=true, .characteristics=(homekit_characteristic_t*[]) {
      &cooler_active,
      &current_temp,
      &current_state,
      &target_state,
      &rotation_speed,
      &heating_threshold,
      &cooling_threshold,
      NULL
    }),
    NULL
  }),
  NULL
};

homekit_server_config_t config = {
  .accessories = accessories,
  .password = "133-71-337"
};
