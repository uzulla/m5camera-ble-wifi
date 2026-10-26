#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// スナップ取得のパラメータ。HTTP のクエリ / BLE のリクエストで共通。
//   w, h   : 撮影サイズ(px)。どちらか片方だけなら 4:3 で補完。rot=90/270 のときは縦横が入れ替わって返る
//   q      : 品質 0..100（大きいほど高画質、0 が最小サイズ）
//   rot    : 時計回りの回転角 0/90/180/270
//   gray   : 1 でグレースケール（サイズがさらに小さくなる）
//   fit    : stretch（既定。縦横比を無視して合わせる）/ crop（中央を切り出す）
typedef struct {
    int w, h;
    int q;
    int rot;
    bool gray;
    bool crop;
} snap_params_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    int w, h;            // 出力画像の実サイズ
    const char *mode;    // "native" = センサー出力そのまま / "soft" = デコード→加工→再エンコード
    void *_fb;
    uint8_t *_buf;
} snap_t;

esp_err_t snap_init(void);
bool snap_ready(void);

void snap_params_default(snap_params_t *p, int w, int h, int q);
// "w=100&h=100&q=20&rot=90&gray=1" 形式を p に上書きで反映する。不正なら ESP_ERR_INVALID_ARG と err にメッセージ
esp_err_t snap_params_parse(const char *query, snap_params_t *p, char *err, size_t errlen);

esp_err_t snap_capture(const snap_params_t *p, snap_t *out, char *err, size_t errlen);
void snap_free(snap_t *s);
