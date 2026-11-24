#pragma once

// Точка доступа Wi-Fi. Пароль по умолчанию: цифры от 1 до 8.
#define APP_WIFI_SSID           "ESP32-SERVER"
#define APP_WIFI_PASSWORD       "12345678"
#define APP_WIFI_CHANNEL        6
#define APP_WIFI_MAX_CLIENTS    4

// Входы модулей работают с логикой 3,3 В. 0: включение LOW; 1: включение HIGH.
// Выключенное состояние до старта программы определяется подтяжками модуля; см. docs/wiring.md.
// Реле вентилятора.
#define APP_FAN_GPIO            25
#define APP_FAN_ACTIVE_LEVEL    1
// MOSFET-модуль светильника.
#define APP_LIGHT_GPIO          26
#define APP_LIGHT_ACTIVE_LEVEL  1

// XGZP6847D: оригинальный протокол 0x6D, диапазон от -100 до 300 кПа (от -1 до 3 бар).
// Значение 0 отключает опрос датчика и скрывает его карточки.
#define APP_SENSOR_ENABLED      1
#define APP_SENSOR_SDA_GPIO     21
#define APP_SENSOR_SCL_GPIO     22
#define APP_SENSOR_I2C_ADDRESS  0x6D
#define APP_SENSOR_INTERNAL_PULLUP 1
#define APP_SENSOR_K            16
#define APP_SENSOR_PERIOD_MS    1000
