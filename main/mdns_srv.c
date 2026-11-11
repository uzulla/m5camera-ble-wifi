#include "mdns_srv.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "wifi_mgr.h"

static const char *TAG = "mdns";

esp_err_t mdns_srv_start(void)
{
    bool ap_on, sta_on;
    wifi_mgr_radio_active(&ap_on, &sta_on);
    if (!ap_on && !sta_on) return ESP_OK;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[7], host[16], inst[16], macs[18];
    snprintf(id, sizeof(id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(host, sizeof(host), "m5cam-%s", id);
    snprintf(inst, sizeof(inst), "M5Cam-%s", id);
    snprintf(macs, sizeof(macs), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_err_t e = mdns_init();
    if (e != ESP_OK) return e;
    mdns_hostname_set(host);
    mdns_instance_name_set(inst);
    mdns_txt_item_t txt[] = {{"id", id}, {"mac", macs}, {"path", "/image"}};
    size_t n = sizeof(txt) / sizeof(txt[0]);
    e = mdns_service_add(NULL, "_m5cam", "_tcp", 80, txt, n);
    if (e == ESP_OK) e = mdns_service_add(NULL, "_http", "_tcp", 80, txt, n);
    if (e != ESP_OK) {
        mdns_free();   // 片方だけ登録された中途半端な状態を残さない
        return e;
    }
    ESP_LOGI(TAG, "%s.local (_m5cam._tcp / _http._tcp, port 80)", host);
    return ESP_OK;
}
