#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool valid;
    esp_err_t error;
    float pressure_bar;
    float temperature_c;
} sensor_reading_t;

esp_err_t sensor_init(void);
void sensor_get_reading(sensor_reading_t *reading);
