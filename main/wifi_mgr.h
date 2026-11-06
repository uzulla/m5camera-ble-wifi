#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_MAX_ENTRIES 8

typedef struct {
    char ssid[33];
    char pass[65];
    int prio;   // 大きいほど優先
} wifi_entry_t;

typedef struct {
    bool connected;
    char ssid[33];
    char ip[16];
    char netmask[16];
    char gw[16];
    char mac[18];
    int rssi;
    uint32_t ip_raw;        // a.b.c.d の順のバイト列（未接続なら 0）
    uint8_t mac_raw[6];
    char target_ssid[33];   // 接続を試みている（または接続中の）SSID。登録がなければ空
} wifi_sta_status_t;

// AP に接続中のクライアント（DHCP で払い出した IP。未割り当てなら ip は空）
typedef struct {
    char mac[18];
    char ip[16];
    int rssi;
} wifi_ap_client_t;

esp_err_t wifi_mgr_start(void);

// NVS に保存された接続先（slot 順）を返す。件数を返す
int wifi_cfg_list(wifi_entry_t *out, int max);
// 同じ SSID があれば上書き、なければ空きスロットに追加
esp_err_t wifi_cfg_set(const char *ssid, const char *pass, int prio);
esp_err_t wifi_cfg_del(const char *ssid);

void wifi_mgr_status(wifi_sta_status_t *st);
// NVS に STA の接続先が 1 件以上あるか
bool wifi_mgr_sta_configured(void);

// AP のビーコン / probe response にベンダー固有 IE（OUI 5a:3c:00, type 1）として payload を載せる。
// いつでも呼べて AP は再起動しない。len は 251 まで
esp_err_t wifi_mgr_set_status_ie(const uint8_t *payload, size_t len);

// AP 側の自分の MAC を buf(18 バイト以上)に書く
void wifi_mgr_ap_mac(char *buf);
// AP に接続中のクライアントを返す。件数を返す
int wifi_mgr_ap_clients(wifi_ap_client_t *out, int max);
