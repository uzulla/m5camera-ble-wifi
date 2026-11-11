#pragma once
#include "esp_err.h"

// mDNS で名前とサービスを広告する（AP / STA の両方のインターフェース）。wifi_mgr_start() の後に呼ぶ。
//   ホスト名 : m5cam-XXXXXX.local（XXXXXX は STA MAC の下位 3 バイト。BLE の広告名 M5Cam-XXXXXX と同じ ID）
//   サービス : _m5cam._tcp（この機体だけを探す用）と _http._tcp、どちらもポート 80
// Wi-Fi が AP / STA ともオフのときは何もしない
esp_err_t mdns_srv_start(void);
