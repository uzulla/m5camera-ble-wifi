#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_snap.h"
#include "cpu_mon.h"
#include "http_srv.h"
#include "i2c_snap.h"
#include "snap.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    cpu_mon_start();
    if (snap_init() != ESP_OK) ESP_LOGE(TAG, "camera unavailable; continuing without it");
    ESP_ERROR_CHECK(wifi_mgr_start());
    ESP_ERROR_CHECK(http_start());
    if (ble_snap_start() != ESP_OK) ESP_LOGE(TAG, "BLE failed to start");
    if (i2c_snap_start() != ESP_OK) ESP_LOGE(TAG, "I2C slave failed to start");
    ESP_LOGI(TAG, "ready");
}
