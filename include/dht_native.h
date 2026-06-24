#ifndef DHT_NATIVE_H
#define DHT_NATIVE_H

#include "esp_err.h"

#define DHT_GPIO_PIN 5

extern volatile float current_temperature;
extern volatile float current_humidity;

void dht_update_current_values();

#endif