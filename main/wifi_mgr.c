#include "wifi_mgr.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"   // httpd_query_key_value（クエリ文字列の解析にだけ使う）
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "wifi";

#define AP_SSID "M5Cam"
#define NS "wifi"
#define BIT_GOT_IP (1 << 0)
#define BIT_DISCONNECTED (1 << 1)
#define BIT_CFG_CHANGED (1 << 2)
#define CONNECT_TIMEOUT_MS 60000   // 接続〜IP 取得まで。モバイルルーターなど DHCP が遅いものがあるので 1 分待つ
#define IDLE_RETRY_MS 10000

static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static volatile int s_cfg_n;            // NVS の登録件数（sta_task が更新）
static char s_target_ssid[33];          // 接続を試みている SSID

// ---- NVS: ssid<i> / pass<i> / prio<i> (i = 0..7) ----

int wifi_cfg_list(wifi_entry_t *out, int max)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    int n = 0;
    for (int i = 0; i < WIFI_MAX_ENTRIES && n < max; i++) {
        char k[16];
        wifi_entry_t *e = &out[n];
        memset(e, 0, sizeof(*e));
        size_t len = sizeof(e->ssid);
        snprintf(k, sizeof(k), "ssid%d", i);
        if (nvs_get_str(h, k, e->ssid, &len) != ESP_OK || !e->ssid[0]) continue;
        len = sizeof(e->pass);
        snprintf(k, sizeof(k), "pass%d", i);
        nvs_get_str(h, k, e->pass, &len);
        int32_t p = 0;
        snprintf(k, sizeof(k), "prio%d", i);
        nvs_get_i32(h, k, &p);
        e->prio = p;
        n++;
    }
    nvs_close(h);
    return n;
}

esp_err_t wifi_cfg_set(const char *ssid, const char *pass, int prio)
{
    if (!ssid || !*ssid || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    int slot = -1, freeslot = -1;
    for (int i = 0; i < WIFI_MAX_ENTRIES; i++) {
        char k[16], v[33];
        size_t len = sizeof(v);
        snprintf(k, sizeof(k), "ssid%d", i);
        if (nvs_get_str(h, k, v, &len) != ESP_OK || !v[0]) {
            if (freeslot < 0) freeslot = i;
        } else if (!strcmp(v, ssid)) {
            slot = i;
        }
    }
    if (slot < 0) slot = freeslot;
    if (slot < 0) { nvs_close(h); return ESP_ERR_NO_MEM; }
    char k[16];
    snprintf(k, sizeof(k), "ssid%d", slot);
    e = nvs_set_str(h, k, ssid);
    snprintf(k, sizeof(k), "pass%d", slot);
    if (e == ESP_OK) e = nvs_set_str(h, k, pass ? pass : "");
    snprintf(k, sizeof(k), "prio%d", slot);
    if (e == ESP_OK) e = nvs_set_i32(h, k, prio);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) xEventGroupSetBits(s_ev, BIT_CFG_CHANGED);
    return e;
}

esp_err_t wifi_cfg_del(const char *ssid)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < WIFI_MAX_ENTRIES; i++) {
        char k[16], v[33];
        size_t len = sizeof(v);
        snprintf(k, sizeof(k), "ssid%d", i);
        if (nvs_get_str(h, k, v, &len) == ESP_OK && !strcmp(v, ssid)) {
            nvs_erase_key(h, k);
            snprintf(k, sizeof(k), "pass%d", i);
            nvs_erase_key(h, k);
            snprintf(k, sizeof(k), "prio%d", i);
            nvs_erase_key(h, k);
            e = nvs_commit(h);
            break;
        }
    }
    nvs_close(h);
    if (e == ESP_OK) xEventGroupSetBits(s_ev, BIT_CFG_CHANGED);
    return e;
}

// ---- クエリ書式（HTTP / BLE 共通）----

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

esp_err_t wifi_cfg_apply_query(const char *query)
{
    char ssid[40] = "", pass[72] = "", v[12];
    if (httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) != ESP_OK || !ssid[0]) return ESP_ERR_INVALID_ARG;
    url_decode(ssid);
    if (httpd_query_key_value(query, "del", v, sizeof(v)) == ESP_OK && atoi(v)) return wifi_cfg_del(ssid);
    httpd_query_key_value(query, "pass", pass, sizeof(pass));
    url_decode(pass);
    int prio = 0;
    if (httpd_query_key_value(query, "prio", v, sizeof(v)) == ESP_OK) prio = atoi(v);
    return wifi_cfg_set(ssid, pass, prio);
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

int wifi_cfg_list_json(char *buf, size_t cap)
{
    wifi_entry_t list[WIFI_MAX_ENTRIES];
    int n = wifi_cfg_list(list, WIFI_MAX_ENTRIES);
    char ssid[80];
    int o = snprintf(buf, cap, "{\"networks\":[");
    for (int i = 0; i < n && o < (int)cap; i++) {
        json_str(ssid, sizeof(ssid), list[i].ssid);
        o += snprintf(buf + o, cap - o, "%s{\"ssid\":\"%s\",\"prio\":%d,\"secured\":%s}", i ? "," : "",
                      ssid, list[i].prio, list[i].pass[0] ? "true" : "false");
    }
    if (o < (int)cap) o += snprintf(buf + o, cap - o, "]}");
    return o < (int)cap ? o : (int)cap - 1;
}

// ---- STA 接続（優先度順にフォールバック）----

static int cmp_prio(const void *a, const void *b)
{
    return ((const wifi_entry_t *)b)->prio - ((const wifi_entry_t *)a)->prio;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW(TAG, "STA disconnected (reason %d)", d->reason);
            xEventGroupClearBits(s_ev, BIT_GOT_IP);
            xEventGroupSetBits(s_ev, BIT_DISCONNECTED);
            break;
        }
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *d = data;
            ESP_LOGI(TAG, "STA associated with \"%.*s\" (ch %d, authmode %d); waiting for DHCP", d->ssid_len, d->ssid, d->channel, d->authmode);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *d = data;
            ESP_LOGI(TAG, "AP: client " MACSTR " joined", MAC2STR(d->mac));
            break;
        }
        default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *d = data;
        ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&d->ip_info.ip));
        xEventGroupClearBits(s_ev, BIT_DISCONNECTED);
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static void sta_task(void *arg)
{
    wifi_entry_t list[WIFI_MAX_ENTRIES];
    for (;;) {
        int n = wifi_cfg_list(list, WIFI_MAX_ENTRIES);
        s_cfg_n = n;
        if (n == 0) {
            s_target_ssid[0] = 0;
            ESP_LOGI(TAG, "no STA credentials in NVS; AP only");
            xEventGroupWaitBits(s_ev, BIT_CFG_CHANGED, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }
        qsort(list, n, sizeof(list[0]), cmp_prio);
        xEventGroupClearBits(s_ev, BIT_CFG_CHANGED);

        bool connected = false;
        for (int i = 0; i < n && !connected; i++) {
            ESP_LOGI(TAG, "STA: trying \"%s\" (prio %d)", list[i].ssid, list[i].prio);
            wifi_config_t c = {0};
            strlcpy(s_target_ssid, list[i].ssid, sizeof(s_target_ssid));
            strlcpy((char *)c.sta.ssid, list[i].ssid, sizeof(c.sta.ssid));
            strlcpy((char *)c.sta.password, list[i].pass, sizeof(c.sta.password));
            c.sta.threshold.authmode = list[i].pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
            xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_DISCONNECTED);
            esp_wifi_set_config(WIFI_IF_STA, &c);
            esp_wifi_connect();
            // 成功（IP 取得）か失敗（切断イベント）のどちらかが来るまで待つ。どちらも来なければタイムアウト
            EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_DISCONNECTED, pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
            connected = b & BIT_GOT_IP;
            if (!connected && !(b & BIT_DISCONNECTED)) {
                esp_wifi_disconnect();   // 認証待ちなどで止まっている接続を中断する
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }
        if (!connected) {
            esp_wifi_disconnect();
            ESP_LOGW(TAG, "STA: all candidates failed; retry in %d s", IDLE_RETRY_MS / 1000);
            xEventGroupWaitBits(s_ev, BIT_CFG_CHANGED, pdFALSE, pdFALSE, pdMS_TO_TICKS(IDLE_RETRY_MS));
            continue;
        }
        // 接続中は切断か設定変更を待ち、どちらでも優先度の先頭からやり直す
        xEventGroupWaitBits(s_ev, BIT_DISCONNECTED | BIT_CFG_CHANGED, pdFALSE, pdFALSE, portMAX_DELAY);
        esp_wifi_disconnect();   // 設定変更で抜けた場合は、いったん切ってからやり直す
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void wifi_mgr_status(wifi_sta_status_t *st)
{
    memset(st, 0, sizeof(*st));
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(st->mac, sizeof(st->mac), MACSTR, MAC2STR(mac));
        memcpy(st->mac_raw, mac, 6);
    }
    strlcpy(st->target_ssid, s_target_ssid, sizeof(st->target_ssid));
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    if ((xEventGroupGetBits(s_ev) & BIT_GOT_IP) && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
        esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
        st->connected = true;
        strlcpy(st->ssid, (char *)ap.ssid, sizeof(st->ssid));
        st->rssi = ap.rssi;
        st->ip_raw = ip.ip.addr;
        snprintf(st->ip, sizeof(st->ip), IPSTR, IP2STR(&ip.ip));
        snprintf(st->netmask, sizeof(st->netmask), IPSTR, IP2STR(&ip.netmask));
        snprintf(st->gw, sizeof(st->gw), IPSTR, IP2STR(&ip.gw));
    }
}

bool wifi_mgr_sta_configured(void) { return s_cfg_n > 0; }

// ---- ベンダー固有 IE（状態の広告）----

#define VND_OUI0 0x5a
#define VND_OUI1 0x3c
#define VND_OUI2 0x00
#define VND_TYPE 0x01
#define VND_MAX_PAYLOAD 251

esp_err_t wifi_mgr_set_status_ie(const uint8_t *payload, size_t len)
{
    if (len > VND_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    static uint8_t buf[sizeof(vendor_ie_data_t) + VND_MAX_PAYLOAD];
    vendor_ie_data_t *ie = (vendor_ie_data_t *)buf;
    ie->element_id = WIFI_VENDOR_IE_ELEMENT_ID;
    ie->length = 4 + len;
    ie->vendor_oui[0] = VND_OUI0;
    ie->vendor_oui[1] = VND_OUI1;
    ie->vendor_oui[2] = VND_OUI2;
    ie->vendor_oui_type = VND_TYPE;
    memcpy(ie->payload, payload, len);
    // 同じスロットに enable=true で再設定しても置き換わらないので、いったん外してから載せる
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, NULL);
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, NULL);
    esp_err_t e = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie);
    if (e == ESP_OK) e = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie);
    if (e != ESP_OK) ESP_LOGW(TAG, "set_vendor_ie: %s", esp_err_to_name(e));
    return e;
}

void wifi_mgr_ap_mac(char *buf)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(buf, 18, MACSTR, MAC2STR(mac));
}

int wifi_mgr_ap_clients(wifi_ap_client_t *out, int max)
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
    int n = list.num < max ? list.num : max;
    esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM] = {0};
    if (n > ESP_WIFI_MAX_CONN_NUM) n = ESP_WIFI_MAX_CONN_NUM;
    for (int i = 0; i < n; i++) memcpy(pairs[i].mac, list.sta[i].mac, 6);
    bool have_ip = esp_netif_dhcps_get_clients_by_mac(s_ap_netif, n, pairs) == ESP_OK;
    for (int i = 0; i < n; i++) {
        snprintf(out[i].mac, sizeof(out[i].mac), MACSTR, MAC2STR(list.sta[i].mac));
        out[i].ip[0] = 0;
        if (have_ip && pairs[i].ip.addr) snprintf(out[i].ip, sizeof(out[i].ip), IPSTR, IP2STR(&pairs[i].ip));
        out[i].rssi = list.sta[i].rssi;
    }
    return n;
}

esp_err_t wifi_mgr_start(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t ap = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP \"%s\" started (192.168.4.1)", AP_SSID);
    return xTaskCreate(sta_task, "wifi_sta", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
