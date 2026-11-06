#pragma once
#include "esp_err.h"

// BLE でスナップを取得する GATT サービス。
//   Service  5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0001
//   REQ      5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0002  Write: "w=160&h=120&q=20&rot=90&gray=1"（HTTP の /jpg と同じクエリ。空でも可）
//   DATA     5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0003  Notify: [seq u16 LE][payload]
//     seq=0     : payload = [JPEG 全長 u32 LE][JPEG の先頭]
//     seq=1..   : payload = JPEG の続き
//     seq=0xFFFF: payload = エラーメッセージ（ASCII）
//   WIFI     5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0004
//     Write: "ssid=HomeAP&pass=secret&prio=10"（HTTP の POST /wifi と同じ。URL エンコード）で NVS に登録 / 更新、
//            "ssid=HomeAP&del=1" で削除。失敗は ATT エラー 0x80(不正) / 0x81(8 件超) / 0x82(見つからない)
//     Read : 登録一覧の JSON（GET /wifi と同じ。パスワードは含まない）
esp_err_t ble_snap_start(void);

// 広告の scan response（Manufacturer Specific Data, 会社 ID 0xFFFF）に載せる状態を更新する。
// 27 バイトを超える分は切る。広告中なら止めて差し替えて再開する（接続には影響しない）
void ble_snap_set_status(const uint8_t *payload, size_t len);
