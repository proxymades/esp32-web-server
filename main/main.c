#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "sensor.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

// Исключены выводы Flash, контакты только для входа, загрузочные контакты и UART прошивки.
#define SAFE_GPIO(p) ((p) == 4 || (p) == 13 || (p) == 14 || (p) == 16 || \
                      (p) == 17 || (p) == 18 || (p) == 19 || (p) == 21 || \
                      (p) == 22 || (p) == 23 || (p) == 25 || (p) == 26 || \
                      (p) == 27 || (p) == 32 || (p) == 33)
_Static_assert(SAFE_GPIO(APP_FAN_GPIO) && SAFE_GPIO(APP_LIGHT_GPIO), "Use safe ESP32 output pins");
_Static_assert(APP_FAN_GPIO != APP_LIGHT_GPIO, "Output GPIOs must differ");
_Static_assert(APP_FAN_ACTIVE_LEVEL == 0 || APP_FAN_ACTIVE_LEVEL == 1, "Invalid fan polarity");
_Static_assert(APP_LIGHT_ACTIVE_LEVEL == 0 || APP_LIGHT_ACTIVE_LEVEL == 1, "Invalid light polarity");
_Static_assert(sizeof(APP_WIFI_SSID) > 1 && sizeof(APP_WIFI_SSID) <= 33, "SSID: 1..32 bytes");
_Static_assert(sizeof(APP_WIFI_PASSWORD) >= 9 && sizeof(APP_WIFI_PASSWORD) <= 64, "Password: 8..63 bytes");
_Static_assert(APP_WIFI_MAX_CLIENTS >= 1 && APP_WIFI_MAX_CLIENTS <= 4, "Use 1..4 Wi-Fi clients");
_Static_assert(APP_WIFI_CHANNEL >= 1 && APP_WIFI_CHANNEL <= 11, "Use Wi-Fi channel 1..11");
#if APP_SENSOR_ENABLED
_Static_assert(SAFE_GPIO(APP_SENSOR_SDA_GPIO) && SAFE_GPIO(APP_SENSOR_SCL_GPIO), "Invalid I2C pins");
_Static_assert(APP_SENSOR_SDA_GPIO != APP_SENSOR_SCL_GPIO, "I2C GPIOs must differ");
_Static_assert(APP_SENSOR_SDA_GPIO != APP_FAN_GPIO && APP_SENSOR_SDA_GPIO != APP_LIGHT_GPIO &&
               APP_SENSOR_SCL_GPIO != APP_FAN_GPIO && APP_SENSOR_SCL_GPIO != APP_LIGHT_GPIO,
               "I2C and control GPIOs must differ");
_Static_assert(APP_SENSOR_K > 0 && APP_SENSOR_PERIOD_MS >= 200, "Invalid sensor configuration");
_Static_assert(APP_SENSOR_INTERNAL_PULLUP == 0 || APP_SENSOR_INTERNAL_PULLUP == 1, "Invalid I2C pull-up setting");
#endif

static const char *TAG = "web_server";
static bool s_fan_on;
static bool s_light_on;
static uint64_t s_commands;
// Состояние выходов и счётчик используются одной задачей HTTP-сервера.
extern const unsigned char index_start[] asm("_binary_index_html_start");
extern const unsigned char index_end[] asm("_binary_index_html_end");

static void relay_init(int pin, int active_level)
{
    // Записать выключенный уровень до включения GPIO на выход.
    ESP_ERROR_CHECK(gpio_set_level(pin, !active_level));
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = active_level == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_level == 1 ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

static esp_err_t send_state(httpd_req_t *req)
{
    wifi_sta_list_t stations = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_ap_get_sta_list(&stations));
    sensor_reading_t sensor;
    sensor_get_reading(&sensor);
    char pressure[32] = "null";
    char temperature[32] = "null";
    if (sensor.valid) {
        snprintf(pressure, sizeof(pressure), "%.5f", sensor.pressure_bar);
        snprintf(temperature, sizeof(temperature), "%.2f", sensor.temperature_c);
    }
    char json[448];
    int size = snprintf(json, sizeof(json),
        "{\"clients\":%d,\"uptime_s\":%" PRIu64 ",\"commands\":%" PRIu64 ","
        "\"fan\":%s,\"light\":%s,\"sensor_enabled\":%s,\"sensor_ok\":%s,"
        "\"pressure_bar\":%s,\"temperature_c\":%s,\"sensor_error\":\"%s\"}",
        stations.num, (uint64_t)(esp_timer_get_time() / 1000000), s_commands,
        s_fan_on ? "true" : "false", s_light_on ? "true" : "false",
        APP_SENSOR_ENABLED ? "true" : "false", sensor.valid ? "true" : "false",
        pressure, temperature, esp_err_to_name(sensor.error));
    if (size < 0 || (size_t)size >= sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "State too large");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, size);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_start, index_end - index_start);
}

static esp_err_t relay_handler(httpd_req_t *req)
{
    char query[64], id[12], on[4];
    size_t length = httpd_req_get_url_query_len(req);
    if (req->content_len != 0 || length == 0 || length >= sizeof(query) ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id, sizeof(id)) != ESP_OK ||
        httpd_query_key_value(query, "on", on, sizeof(on)) != ESP_OK ||
        (strcmp(on, "0") != 0 && strcmp(on, "1") != 0)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use id=fan|light&on=0|1");
    }
    bool *state;
    int pin, active_level;
    if (strcmp(id, "fan") == 0) {
        state = &s_fan_on;
        pin = APP_FAN_GPIO;
        active_level = APP_FAN_ACTIVE_LEVEL;
    } else if (strcmp(id, "light") == 0) {
        state = &s_light_on;
        pin = APP_LIGHT_GPIO;
        active_level = APP_LIGHT_ACTIVE_LEVEL;
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown output");
    }
    bool next = strcmp(on, "1") == 0;
    const int level = next ? active_level : !active_level;
    esp_err_t err = gpio_set_level(pin, level);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "GPIO error");
    }
    *state = next;
    ++s_commands;
    ESP_LOGI(TAG, "%s: %s, GPIO%d=%s, commands=%" PRIu64,
             id, next ? "ON" : "OFF", pin, level ? "HIGH" : "LOW", s_commands);
    return send_state(req);
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    assert(ap);
    // Задать фиксированный адрес независимо от настроек SDK по умолчанию.
    esp_netif_ip_info_t ip = {0};
    ESP_ERROR_CHECK(esp_netif_str_to_ip4("192.168.4.1", &ip.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4("192.168.4.1", &ip.gw));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4("255.255.255.0", &ip.netmask));
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t config = {
        .ap = {
            .ssid = APP_WIFI_SSID,
            .ssid_len = sizeof(APP_WIFI_SSID) - 1,
            .password = APP_WIFI_PASSWORD,
            .channel = APP_WIFI_CHANNEL,
            .max_connection = APP_WIFI_MAX_CLIENTS,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/api/state", .method = HTTP_GET, .handler = send_state},
        {.uri = "/api/relay", .method = HTTP_POST, .handler = relay_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
}

void app_main(void)
{
    // Инициализировать выходы первыми. Состояние ON после перезапуска не восстанавливается.
    relay_init(APP_FAN_GPIO, APP_FAN_ACTIVE_LEVEL);
    relay_init(APP_LIGHT_GPIO, APP_LIGHT_ACTIVE_LEVEL);
    ESP_LOGI(TAG, "Fan GPIO%d: ON=%s, OFF=%s", APP_FAN_GPIO,
             APP_FAN_ACTIVE_LEVEL ? "HIGH" : "LOW", APP_FAN_ACTIVE_LEVEL ? "LOW" : "HIGH");
    ESP_LOGI(TAG, "Light GPIO%d: ON=%s, OFF=%s", APP_LIGHT_GPIO,
             APP_LIGHT_ACTIVE_LEVEL ? "HIGH" : "LOW", APP_LIGHT_ACTIVE_LEVEL ? "LOW" : "HIGH");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(sensor_init());
    wifi_init();
    web_server_start();
    ESP_LOGI(TAG, "Wi-Fi: %s | Open http://192.168.4.1", APP_WIFI_SSID);
    ESP_LOGI(TAG, "Fan GPIO%d, light GPIO%d; both OFF", APP_FAN_GPIO, APP_LIGHT_GPIO);
}
