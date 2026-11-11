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
//   fmt    : jpg（既定）/ pgm（gray8。gray=1 扱いになる）/ ppm（RGB24）/ bgra（ヘッダなしの生 BGRA、A=255）
//            pgm / ppm は 1 行のヘッダ "P5 <w> <h> 255\n" / "P6 <w> <h> 255\n" の後に生バイトが続く。q は無視される
typedef enum { SNAP_FMT_JPEG = 0, SNAP_FMT_PGM, SNAP_FMT_PPM, SNAP_FMT_BGRA } snap_fmt_t;

typedef struct {
    int w, h;
    int q;
    int rot;
    bool gray;
    bool crop;
    snap_fmt_t fmt;
} snap_params_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    int w, h;            // 出力画像の実サイズ
    const char *mode;    // "native" = センサー出力そのまま / "soft" = デコード→加工→再エンコード / "raw" = デコード→加工→非圧縮
    snap_fmt_t fmt;
    const char *mime;    // HTTP の Content-Type
    const char *fmt_name; // "jpg" / "pgm" / "ppm" / "bgra"
    void *_fb;
    uint8_t *_buf;
} snap_t;

esp_err_t snap_init(void);
bool snap_ready(void);
// 撮影（native / soft）の実行中か
bool snap_busy(void);

void snap_params_default(snap_params_t *p, int w, int h, int q);
// "w=100&h=100&q=20&rot=90&gray=1" 形式を p に上書きで反映する。不正なら ESP_ERR_INVALID_ARG と err にメッセージ
esp_err_t snap_params_parse(const char *query, snap_params_t *p, char *err, size_t errlen);

esp_err_t snap_capture(const snap_params_t *p, snap_t *out, char *err, size_t errlen);
void snap_free(snap_t *s);
