#pragma once
#include "esp_err.h"

// BLE でスナップを取得する GATT サービス。広告名は "M5Cam-" + STA MAC 下位 3 バイト（例 M5Cam-445566）。
// 128bit のサービス UUID は広告に載せていない（名前と状態で 62 バイトを使い切るため）。クライアントは名前の前方一致で探す。
//   Service  5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0001
//   REQ      5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0002  Write: "w=160&h=120&q=20&rot=90&gray=1"（HTTP の /image と同じクエリ。空でも可）
//   DATA     5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0003  Notify: [seq u16 LE][payload]
//     seq=0     : payload = [JPEG 全長 u32 LE][JPEG の先頭]
//     seq=1..   : payload = JPEG の続き
//     seq=0xFFFF: payload = エラーメッセージ（ASCII）
//   WIFI     5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0004
//     Write: "ssid=HomeAP&pass=secret&prio=10"（HTTP の POST /wifi と同じ。URL エンコード）で NVS に登録 / 更新、
//            "ssid=HomeAP&del=1" で削除。失敗は ATT エラー 0x80(不正) / 0x81(8 件超) / 0x82(見つからない)
//     Read : 登録一覧の JSON（GET /wifi と同じ。パスワードは含まない）
//   STATUS   5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0005
//     Read : 状態の JSON（/status の簡略版。STA の IP、接続先、カメラの可否など）。広告をスキャンできないクライアント（Web Bluetooth）向け
//   CTRL     5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0006
//     Write: "ap=0&sta=1" で AP / STA のオンオフを NVS に保存（次の起動から有効）、"reboot=1" で再起動（組み合わせ可）
//     Read : {"ap":true,"sta":false,"active":{"ap":true,"sta":true}}（保存値と、今動いている状態）
esp_err_t ble_snap_start(void);

// 広告の scan response（Manufacturer Specific Data, 会社 ID 0xFFFF）に載せる状態を更新する。
// 27 バイトを超える分は切る。広告中なら止めて差し替えて再開する（接続には影響しない）
void ble_snap_set_status(const uint8_t *payload, size_t len);
