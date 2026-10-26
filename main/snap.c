#include "snap.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "img_converters.h"

static const char *TAG = "snap";

// M5Stack Camera (PSRAM 版) のピン配置
#define PIN_PWDN  -1
#define PIN_RESET 15
#define PIN_XCLK  27
#define PIN_SIOD  22
#define PIN_SIOC  23
#define PIN_D7    19
#define PIN_D6    36
#define PIN_D5    18
#define PIN_D4    39
#define PIN_D3     5
#define PIN_D2    34
#define PIN_D1    35
#define PIN_D0    32
#define PIN_VSYNC 25
#define PIN_HREF  26
#define PIN_PCLK  21

#define DEFAULT_W 640
#define DEFAULT_H 480
#define SOFT_MAX_W 800   // soft 経路で扱える加工前の最大サイズ（PSRAM に収めるため）
#define SOFT_MAX_H 600
#define SOFT_SENSOR_Q 10

typedef struct { int w, h; framesize_t fs; } fsize_t;
static const fsize_t SIZES[] = {
    {160, 120, FRAMESIZE_QQVGA},
    {240, 176, FRAMESIZE_HQVGA},
    {320, 240, FRAMESIZE_QVGA},
    {400, 296, FRAMESIZE_CIF},
    {640, 480, FRAMESIZE_VGA},
    {800, 600, FRAMESIZE_SVGA},
    {1024, 768, FRAMESIZE_XGA},
    {1280, 1024, FRAMESIZE_SXGA},
    {1600, 1200, FRAMESIZE_UXGA},
};
#define N_SIZES (sizeof(SIZES) / sizeof(SIZES[0]))

static SemaphoreHandle_t s_lock;
static bool s_ready;
static struct { int fs, q, flip; } s_cur = {-1, -1, -1};

void snap_params_default(snap_params_t *p, int w, int h, int q)
{
    memset(p, 0, sizeof(*p));
    p->w = w;
    p->h = h;
    p->q = q;
}

static bool get_int(const char *query, const char *key, int *out, char *err, size_t errlen)
{
    char v[24];
    if (httpd_query_key_value(query, key, v, sizeof(v)) != ESP_OK) return false;
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end) {
        snprintf(err, errlen, "invalid %s", key);
        *out = INT32_MIN;
        return true;
    }
    *out = (int)n;
    return true;
}

esp_err_t snap_params_parse(const char *query, snap_params_t *p, char *err, size_t errlen)
{
    err[0] = 0;
    if (!query || !*query) return ESP_OK;
    int v, w = 0, h = 0;
    bool hw = false, hh = false;
    if (get_int(query, "w", &v, err, errlen)) { w = v; hw = true; }
    if (get_int(query, "h", &v, err, errlen)) { h = v; hh = true; }
    if (get_int(query, "q", &v, err, errlen)) p->q = v;
    if (get_int(query, "rot", &v, err, errlen) || get_int(query, "r", &v, err, errlen)) p->rot = v;
    if (get_int(query, "gray", &v, err, errlen)) p->gray = v != 0;
    char s[16];
    if (httpd_query_key_value(query, "fit", s, sizeof(s)) == ESP_OK) {
        if (!strcmp(s, "crop")) p->crop = true;
        else if (!strcmp(s, "stretch")) p->crop = false;
        else snprintf(err, errlen, "fit must be stretch or crop");
    }
    if (err[0]) return ESP_ERR_INVALID_ARG;

    if (hw && !hh) h = w * 3 / 4;
    if (hh && !hw) w = h * 4 / 3;
    if (hw || hh) { p->w = w; p->h = h; }
    if (p->w < 1 || p->h < 1 || p->w > 1600 || p->h > 1200) {
        snprintf(err, errlen, "w/h must be 1..1600 / 1..1200");
        return ESP_ERR_INVALID_ARG;
    }
    if (p->q < 0 || p->q > 100) {
        snprintf(err, errlen, "q must be 0..100");
        return ESP_ERR_INVALID_ARG;
    }
    int r = ((p->rot % 360) + 360) % 360;
    if (r % 90) {
        snprintf(err, errlen, "rot must be 0/90/180/270");
        return ESP_ERR_INVALID_ARG;
    }
    p->rot = r;
    return ESP_OK;
}

esp_err_t snap_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    camera_config_t c = {
        .pin_pwdn = PIN_PWDN, .pin_reset = PIN_RESET, .pin_xclk = PIN_XCLK,
        .pin_sccb_sda = PIN_SIOD, .pin_sccb_scl = PIN_SIOC,
        .pin_d7 = PIN_D7, .pin_d6 = PIN_D6, .pin_d5 = PIN_D5, .pin_d4 = PIN_D4,
        .pin_d3 = PIN_D3, .pin_d2 = PIN_D2, .pin_d1 = PIN_D1, .pin_d0 = PIN_D0,
        .pin_vsync = PIN_VSYNC, .pin_href = PIN_HREF, .pin_pclk = PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_UXGA,   // バッファは最大サイズで確保し、実際は都度切り替える
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t e = esp_camera_init(&c);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: 0x%x", e);
        return e;
    }
    sensor_t *s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "camera ready: sensor PID 0x%x", s->id.PID);
    s_ready = true;
    return ESP_OK;
}

bool snap_ready(void) { return s_ready; }

static int pick_size(int w, int h)
{
    for (int i = 0; i < (int)N_SIZES; i++)
        if (SIZES[i].w >= w && SIZES[i].h >= h) return i;
    return -1;
}

static int sensor_q(int q)
{
    int v = 63 - (q * 59) / 100;   // q=100 → 4, q=0 → 63
    return v < 4 ? 4 : v;
}

// センサー設定を切り替える。変わったときは古いフレームを捨てる
static void apply_sensor(int fs_idx, int sq, int flip)
{
    sensor_t *s = esp_camera_sensor_get();
    bool changed = false;
    if (s_cur.fs != fs_idx) { s->set_framesize(s, SIZES[fs_idx].fs); s_cur.fs = fs_idx; changed = true; }
    if (s_cur.q != sq) { s->set_quality(s, sq); s_cur.q = sq; changed = true; }
    if (s_cur.flip != flip) { s->set_hmirror(s, flip); s->set_vflip(s, flip); s_cur.flip = flip; changed = true; }
    if (changed) {
        for (int i = 0; i < 3; i++) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) esp_camera_fb_return(fb);
        }
    }
}

// jpg2rgb565 の出力はリトルエンディアン（fmt2jpg が期待するのはビッグエンディアン）
static inline uint8_t luma565(const uint8_t *p)
{
    uint16_t v = (p[1] << 8) | p[0];
    int r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
    return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
}

static esp_err_t capture_native(const snap_params_t *p, int idx, snap_t *out, char *err, size_t el)
{
    apply_sensor(idx, sensor_q(p->q), p->rot == 180);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { snprintf(err, el, "capture failed"); return ESP_FAIL; }
    out->data = fb->buf;
    out->len = fb->len;
    out->w = fb->width;
    out->h = fb->height;
    out->mode = "native";
    out->_fb = fb;
    return ESP_OK;
}

static esp_err_t capture_soft(const snap_params_t *p, snap_t *out, char *err, size_t el)
{
    // w×h は撮影サイズ。90/270 のときは縦横が入れ替わった画像を返す
    bool odd = p->rot == 90 || p->rot == 270;
    int pw = p->w, ph = p->h;
    int fw = odd ? ph : pw, fh = odd ? pw : ph;   // 出力サイズ
    int idx = pick_size(pw, ph);
    if (idx < 0 || SIZES[idx].w > SOFT_MAX_W || SIZES[idx].h > SOFT_MAX_H) {
        snprintf(err, el, "size too large for resize/rotate/gray (max %dx%d before rotation); native sizes without rot/gray are fine",
                 SOFT_MAX_W, SOFT_MAX_H);
        return ESP_ERR_INVALID_ARG;
    }

    apply_sensor(idx, SOFT_SENSOR_Q, 0);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { snprintf(err, el, "capture failed"); return ESP_FAIL; }

    // 目的サイズ以上を保てる最大の縮小率でデコードする
    int sc = 0;
    while (sc < 3 && (fb->width >> (sc + 1)) >= pw && (fb->height >> (sc + 1)) >= ph) sc++;
    int dw = fb->width >> sc, dh = fb->height >> sc;
    uint8_t *dec = heap_caps_malloc((size_t)dw * dh * 2, MALLOC_CAP_SPIRAM);
    if (!dec) { esp_camera_fb_return(fb); snprintf(err, el, "no memory"); return ESP_ERR_NO_MEM; }
    bool ok = jpg2rgb565(fb->buf, fb->len, dec, (esp_jpeg_image_scale_t)sc);
    esp_camera_fb_return(fb);
    if (!ok) { free(dec); snprintf(err, el, "decode failed"); return ESP_FAIL; }

    // 切り出し範囲（crop のとき中央を、縦横比が合うように切り出す）
    int cx0 = 0, cy0 = 0, cw = dw, ch = dh;
    if (p->crop) {
        if ((long)dw * ph > (long)dh * pw) { cw = (int)((long)dh * pw / ph); cx0 = (dw - cw) / 2; }
        else { ch = (int)((long)dw * ph / pw); cy0 = (dh - ch) / 2; }
    }
    int *sx = malloc(sizeof(int) * pw), *sy = malloc(sizeof(int) * ph);
    int bpp = p->gray ? 1 : 2;
    uint8_t *img = heap_caps_malloc((size_t)fw * fh * bpp, MALLOC_CAP_SPIRAM);
    if (!sx || !sy || !img) {
        free(sx); free(sy); free(img); free(dec);
        snprintf(err, el, "no memory");
        return ESP_ERR_NO_MEM;
    }
    for (int u = 0; u < pw; u++) sx[u] = cx0 + u * cw / pw;
    for (int v = 0; v < ph; v++) sy[v] = cy0 + v * ch / ph;

    for (int y = 0; y < fh; y++) {
        for (int x = 0; x < fw; x++) {
            int u, v;
            switch (p->rot) {
            case 90:  u = y;           v = ph - 1 - x; break;
            case 180: u = pw - 1 - x;  v = ph - 1 - y; break;
            case 270: u = pw - 1 - y;  v = x;          break;
            default:  u = x;           v = y;          break;
            }
            const uint8_t *src = dec + ((size_t)sy[v] * dw + sx[u]) * 2;
            size_t o = (size_t)y * fw + x;
            if (p->gray) {
                img[o] = luma565(src);
            } else {
                img[o * 2] = src[1];
                img[o * 2 + 1] = src[0];
            }
        }
    }
    free(sx); free(sy); free(dec);

    uint8_t *jpg = NULL;
    size_t jlen = 0;
    ok = fmt2jpg(img, (size_t)fw * fh * bpp, fw, fh,
                 p->gray ? PIXFORMAT_GRAYSCALE : PIXFORMAT_RGB565, p->q < 1 ? 1 : p->q, &jpg, &jlen);
    free(img);
    if (!ok || !jpg) { snprintf(err, el, "encode failed"); return ESP_FAIL; }
    out->data = jpg;
    out->len = jlen;
    out->w = fw;
    out->h = fh;
    out->mode = "soft";
    out->_buf = jpg;
    return ESP_OK;
}

esp_err_t snap_capture(const snap_params_t *p, snap_t *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    if (!s_ready) { snprintf(err, errlen, "camera not ready"); return ESP_ERR_INVALID_STATE; }
    int idx = pick_size(p->w, p->h);
    bool native = !p->gray && (p->rot == 0 || p->rot == 180) && idx >= 0 &&
                  SIZES[idx].w == p->w && SIZES[idx].h == p->h;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = native ? capture_native(p, idx, out, err, errlen) : capture_soft(p, out, err, errlen);
    xSemaphoreGive(s_lock);
    return e;
}

void snap_free(snap_t *s)
{
    if (s->_fb) esp_camera_fb_return(s->_fb);
    free(s->_buf);
    memset(s, 0, sizeof(*s));
}
