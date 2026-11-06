#include "status_adv.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_snap.h"
#include "snap.h"
#include "wifi_mgr.h"

static const char *TAG = "status_adv";

size_t status_adv_build(uint8_t *buf, size_t cap)
{
    if (cap < STATUS_ADV_FIXED) return 0;
    wifi_sta_status_t st;
    wifi_mgr_status(&st);
    wifi_ap_client_t cl[4];
    int ncl = wifi_mgr_ap_clients(cl, 4);

    uint8_t flags = 0;
    if (st.connected) flags |= STATUS_F_STA_CONNECTED;
    if (wifi_mgr_sta_configured()) flags |= STATUS_F_STA_CONFIGURED;
    if (snap_ready()) flags |= STATUS_F_CAMERA_OK;
    if (ncl > 0) flags |= STATUS_F_AP_CLIENTS;
    if (snap_busy()) flags |= STATUS_F_BUSY;
    bool ap_on, sta_on;
    wifi_mgr_radio_active(&ap_on, &sta_on);
    if (!ap_on) flags |= STATUS_F_AP_OFF;
    if (!sta_on) flags |= STATUS_F_STA_OFF;

    uint32_t up_min = (uint32_t)(esp_timer_get_time() / 60000000LL);
    if (up_min > 0xFFFF) up_min = 0xFFFF;
    size_t heap_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    if (heap_kb > 255) heap_kb = 255;

    buf[0] = STATUS_ADV_VER;
    buf[1] = flags;
    memcpy(buf + 2, &st.ip_raw, 4);
    memcpy(buf + 6, st.mac_raw + 3, 3);
    buf[9] = (uint8_t)(int8_t)(st.connected ? st.rssi : 0);
    buf[10] = up_min & 0xFF;
    buf[11] = (up_min >> 8) & 0xFF;
    buf[12] = (uint8_t)heap_kb;
    buf[13] = (uint8_t)(ncl > 255 ? 255 : ncl);

    const char *ssid = st.connected ? st.ssid : st.target_ssid;
    size_t n = strnlen(ssid, 32);
    if (n > cap - STATUS_ADV_FIXED) n = cap - STATUS_ADV_FIXED;
    memcpy(buf + STATUS_ADV_FIXED, ssid, n);
    return STATUS_ADV_FIXED + n;
}

static void json_str(char *dst, size_t len, const char *s)
{
    size_t o = 0;
    for (; *s && o + 3 < len; s++) {
        if (*s == '"' || *s == '\\') dst[o++] = '\\';
        if ((unsigned char)*s < 0x20) continue;
        dst[o++] = *s;
    }
    dst[o] = 0;
}

int status_adv_json(char *buf, size_t cap)
{
    wifi_sta_status_t st;
    wifi_mgr_status(&st);
    wifi_ap_client_t cl[4];
    int ncl = wifi_mgr_ap_clients(cl, 4);
    char ssid[80], target[80], radio[96];
    json_str(ssid, sizeof(ssid), st.ssid);
    json_str(target, sizeof(target), st.target_ssid);
    wifi_radio_json(radio, sizeof(radio));
    int o = snprintf(buf, cap,
                     "{\"id\":\"%02x%02x%02x\",\"uptime_s\":%lld,\"camera\":%s,\"busy\":%s,\"free_heap\":%u,\"radio\":%s,"
                     "\"ap\":{\"ip\":\"192.168.4.1\",\"clients\":%d},"
                     "\"sta\":{\"configured\":%s,\"connected\":%s,\"ssid\":\"%s\",\"target_ssid\":\"%s\",\"ip\":\"%s\",\"gw\":\"%s\",\"rssi\":%d}}",
                     st.mac_raw[3], st.mac_raw[4], st.mac_raw[5], esp_timer_get_time() / 1000000LL,
                     snap_ready() ? "true" : "false", snap_busy() ? "true" : "false",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), radio, ncl,
                     wifi_mgr_sta_configured() ? "true" : "false", st.connected ? "true" : "false",
                     ssid, target, st.ip, st.gw, st.connected ? st.rssi : 0);
    return o < (int)cap ? o : (int)cap - 1;
}

static void task(void *arg)
{
    uint8_t last[STATUS_ADV_MAX];
    size_t last_len = 0;
    bool first = true;
    for (;;) {
        uint8_t cur[STATUS_ADV_MAX];
        size_t n = status_adv_build(cur, sizeof(cur));
        if (first || n != last_len || memcmp(cur, last, n) != 0) {
            ble_snap_set_status(cur, n);
            wifi_mgr_set_status_ie(cur, n);
            memcpy(last, cur, n);
            last_len = n;
            if (first) ESP_LOGI(TAG, "broadcasting status (%u bytes)", (unsigned)n);
            first = false;
        }
        vTaskDelay(pdMS_TO_TICKS(STATUS_ADV_PERIOD_MS));
    }
}

esp_err_t status_adv_start(void)
{
    return xTaskCreate(task, "status_adv", 4096, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
