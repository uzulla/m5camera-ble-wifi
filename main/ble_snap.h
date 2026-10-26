#pragma once
#include "esp_err.h"

// BLE でスナップを取得する GATT サービス。
//   Service  5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0001
//   REQ      5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0002  Write: "w=160&h=120&q=20&rot=90&gray=1"（HTTP の /jpg と同じクエリ。空でも可）
//   DATA     5a3c0000-0b4e-4d6a-9f2e-7c1a8e5b0003  Notify: [seq u16 LE][payload]
//     seq=0     : payload = [JPEG 全長 u32 LE][JPEG の先頭]
//     seq=1..   : payload = JPEG の続き
//     seq=0xFFFF: payload = エラーメッセージ（ASCII）
esp_err_t ble_snap_start(void);
