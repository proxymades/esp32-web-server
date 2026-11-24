#include "sensor.h"
#include "app_config.h"

#if APP_SENSOR_ENABLED
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static SemaphoreHandle_t s_lock;
static sensor_reading_t s_reading = {.error = ESP_ERR_NOT_FINISHED};

static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t size)
{
    return i2c_master_transmit_receive(s_device, &reg, 1, data, size, 30);
}

static esp_err_t measure(sensor_reading_t *reading)
{
    esp_err_t err = i2c_master_probe(s_bus, APP_SENSOR_I2C_ADDRESS, 30);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t command[] = {0x30, 0x0A};
    err = i2c_master_transmit(s_device, command, sizeof(command), 30);
    if (err != ESP_OK) {
        return err;
    }
    const int64_t deadline = esp_timer_get_time() + 100000;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t status;
        err = read_register(0x30, &status, 1);
        if (err != ESP_OK) {
            return err;
        }
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        if (!(status & 0x08)) {
            break;
        }
    }
    uint8_t bytes[5];
    err = read_register(0x06, bytes, sizeof(bytes));
    if (err != ESP_OK) {
        return err;
    }
    int32_t pressure = ((uint32_t)bytes[0] << 16) |
                       ((uint32_t)bytes[1] << 8) | bytes[2];
    if (pressure & 0x800000) {
        pressure -= 0x1000000;
    }
    int32_t temperature = ((uint16_t)bytes[3] << 8) | bytes[4];
    if (temperature & 0x8000) {
        temperature -= 0x10000;
    }
    reading->pressure_bar = (float)pressure / APP_SENSOR_K / 100000.0f;
    reading->temperature_c = (float)temperature / 256.0f;
    reading->valid = true;
    return ESP_OK;
}

static void sensor_task(void *arg)
{
    (void)arg;
    esp_err_t previous_error = ESP_ERR_NOT_FINISHED;
    // Пауза после включения питания перед первым измерением.
    vTaskDelay(pdMS_TO_TICKS(100));
    TickType_t next = xTaskGetTickCount();
    while (true) {
        sensor_reading_t reading = {0};
        esp_err_t err = measure(&reading);
        reading.error = err;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_reading = reading; // При ошибке предыдущие показания считаются недействительными.
        xSemaphoreGive(s_lock);
        if (err != previous_error) {
            if (err == ESP_OK) {
                ESP_LOGI("sensor", "XGZP6847D 0x%02X: readings available", APP_SENSOR_I2C_ADDRESS);
            } else {
                ESP_LOGW("sensor", "XGZP6847D 0x%02X, SDA GPIO%d, SCL GPIO%d: %s",
                         APP_SENSOR_I2C_ADDRESS, APP_SENSOR_SDA_GPIO,
                         APP_SENSOR_SCL_GPIO, esp_err_to_name(err));
            }
            previous_error = err;
        }
        xTaskDelayUntil(&next, pdMS_TO_TICKS(APP_SENSOR_PERIOD_MS));
    }
}

esp_err_t sensor_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = APP_SENSOR_SDA_GPIO,
        .scl_io_num = APP_SENSOR_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = APP_SENSOR_INTERNAL_PULLUP,
    };
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) {
        return err;
    }
    i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = APP_SENSOR_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &device, &s_device);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI("sensor", "I2C 100 kHz, SDA GPIO%d, SCL GPIO%d, internal pull-ups %s",
             APP_SENSOR_SDA_GPIO, APP_SENSOR_SCL_GPIO, APP_SENSOR_INTERNAL_PULLUP ? "ON" : "OFF");
    return xTaskCreate(sensor_task, "pressure", 3072, NULL, 4, NULL) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}

void sensor_get_reading(sensor_reading_t *reading)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *reading = s_reading;
    xSemaphoreGive(s_lock);
}
#else
esp_err_t sensor_init(void)
{
    return ESP_OK;
}

void sensor_get_reading(sensor_reading_t *reading)
{
    *reading = (sensor_reading_t){0};
}
#endif
