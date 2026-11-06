#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// 画面のない機体の状態を、接続なしで読める場所に撒く。
//   - BLE 広告の scan response（Manufacturer Specific Data, 会社 ID 0xFFFF）
//   - Wi-Fi AP のビーコン / probe response のベンダー固有 IE（OUI 5a:3c:00, type 1）
// どちらも同じバイト列（BLE は 27 バイトに切り詰めるので SSID が 13 バイトまで）。
//
//   オフセット サイズ 内容
//    0   1  ver = 1
//    1   1  flags（STATUS_F_*）
//    2   4  STA の IPv4（a.b.c.d の順。未接続なら 0）
//    6   3  STA MAC の下位 3 バイト（個体識別）
//    9   1  STA の RSSI (int8)。未接続なら 0
//   10   2  稼働時間（分, u16 LE）
//   12   1  空き内部ヒープ (KiB, 255 で飽和)
//   13   1  AP に接続中のクライアント数
//   14   -  SSID（接続中はその SSID、未接続なら接続を試みている SSID。NUL なし）
#define STATUS_ADV_VER 1
#define STATUS_F_STA_CONNECTED 0x01   // STA が IP を取得している
#define STATUS_F_STA_CONFIGURED 0x02  // NVS に STA の接続先がある
#define STATUS_F_CAMERA_OK 0x04
#define STATUS_F_AP_CLIENTS 0x08      // AP にクライアントがいる
#define STATUS_F_BUSY 0x10            // 撮影中
#define STATUS_ADV_FIXED 14
#define STATUS_ADV_MAX (STATUS_ADV_FIXED + 32)
#define STATUS_ADV_PERIOD_MS 5000

// 現在の状態を buf に詰めて長さを返す。cap が足りなければ SSID を切る
size_t status_adv_build(uint8_t *buf, size_t cap);
// 定期的に状態を組み立て、変化があれば BLE と Wi-Fi に反映するタスクを開始する
esp_err_t status_adv_start(void);
