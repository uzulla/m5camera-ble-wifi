#include "i2c_snap.h"

#include <string.h>

#include "driver/i2c_slave.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "snap.h"

static const char *TAG = "i2c";

#define I2C_ADDR 0x42
#define PIN_SDA 4
#define PIN_SCL 13
#define CMD_MAX 128
#define CHUNK_MAX 120
#define I2C_DEF_W 160
#define I2C_DEF_H 120
#define I2C_DEF_Q 20

#define CMD_SNAP 0x01
#define CMD_STATUS 0x02
#define CMD_READ 0x03
#define SYNC 0xA5

enum { ST_NONE = 0, ST_BUSY = 1, ST_READY = 2, ST_ERROR = 3 };

typedef struct {
    uint8_t len;
    uint8_t d[CMD_MAX];
} cmd_t;

static i2c_slave_dev_handle_t s_dev;
static QueueHandle_t s_cmdq;   // ISR → コマンド処理タスク
static QueueHandle_t s_capq;   // コマンド処理タスク → 撮影タスク
static volatile uint8_t s_state = ST_NONE;
static snap_t s_snap;

static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 0x80) ? (c << 1) ^ 0x07 : c << 1;
    }
    return c;
}

// STOP 条件でトランザクション単位に呼ばれる（ISR）
static IRAM_ATTR bool on_receive(i2c_slave_dev_handle_t dev, const i2c_slave_rx_done_event_data_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    cmd_t c;
    uint32_t n = e->length > CMD_MAX ? CMD_MAX : e->length;
    c.len = n;
    for (uint32_t i = 0; i < n; i++) c.d[i] = e->buffer[i];
    xQueueSendFromISR(s_cmdq, &c, &woken);
    return woken;
}

static void respond(const uint8_t *buf, size_t len)
{
    uint32_t w = 0;
    i2c_slave_write(s_dev, buf, len, &w, 100);
}

static void send_status(void)
{
    uint8_t f[11];
    uint8_t st = s_state;
    uint32_t total = st == ST_READY ? s_snap.len : 0;
    uint16_t w = st == ST_READY ? s_snap.w : 0, h = st == ST_READY ? s_snap.h : 0;
    f[0] = SYNC;
    f[1] = st;
    memcpy(f + 2, &total, 4);
    memcpy(f + 6, &w, 2);
    memcpy(f + 8, &h, 2);
    f[10] = crc8(f + 1, 9);
    respond(f, sizeof(f));
}

static void send_data(const uint8_t *arg)
{
    uint32_t off;
    memcpy(&off, arg, 4);
    uint32_t n = arg[4];
    uint8_t f[CHUNK_MAX + 3];
    if (s_state != ST_READY || off >= s_snap.len) n = 0;
    else {
        if (n > CHUNK_MAX) n = CHUNK_MAX;
        if (n > s_snap.len - off) n = s_snap.len - off;
    }
    f[0] = SYNC;
    f[1] = n;
    if (n) memcpy(f + 2, s_snap.data + off, n);
    f[2 + n] = crc8(f + 1, 1 + n);
    respond(f, n + 3);
}

static void cmd_task(void *arg)
{
    cmd_t c;
    for (;;) {
        xQueueReceive(s_cmdq, &c, portMAX_DELAY);
        if (c.len == 0) continue;
        switch (c.d[0]) {
        case CMD_SNAP:
            if (s_state == ST_BUSY) break;
            s_state = ST_BUSY;
            c.d[c.len < CMD_MAX ? c.len : CMD_MAX - 1] = 0;   // クエリ文字列の終端
            xQueueSend(s_capq, c.d + 1, 0);
            break;
        case CMD_STATUS:
            send_status();
            break;
        case CMD_READ:
            if (c.len >= 6) send_data(c.d + 1);
            break;
        default:
            ESP_LOGW(TAG, "unknown command 0x%02x", c.d[0]);
        }
    }
}

static void capture_task(void *arg)
{
    char q[CMD_MAX], err[160];
    for (;;) {
        xQueueReceive(s_capq, q, portMAX_DELAY);
        snap_free(&s_snap);
        snap_params_t p;
        snap_params_default(&p, I2C_DEF_W, I2C_DEF_H, I2C_DEF_Q);
        if (snap_params_parse(q, &p, err, sizeof(err)) != ESP_OK || snap_capture(&p, &s_snap, err, sizeof(err)) != ESP_OK) {
            ESP_LOGW(TAG, "snap failed: %s", err);
            s_state = ST_ERROR;
            continue;
        }
        ESP_LOGI(TAG, "snap ready: %u bytes (%dx%d, %s)", (unsigned)s_snap.len, s_snap.w, s_snap.h, s_snap.mode);
        s_state = ST_READY;
    }
}

esp_err_t i2c_snap_start(void)
{
    s_cmdq = xQueueCreate(8, sizeof(cmd_t));
    s_capq = xQueueCreate(1, CMD_MAX - 1);   // c.d[1..] のクエリ分
    i2c_slave_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = 256,
        .receive_buf_depth = 256,
        .slave_addr = I2C_ADDR,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .flags.enable_internal_pullup = true,   // 外付けのプルアップがなくても動くように（400kHz では外付けを推奨）
    };
    esp_err_t e = i2c_new_slave_device(&cfg, &s_dev);
    if (e != ESP_OK) return e;
    i2c_slave_event_callbacks_t cbs = {.on_receive = on_receive};
    e = i2c_slave_register_event_callbacks(s_dev, &cbs, NULL);
    if (e != ESP_OK) return e;
    xTaskCreate(cmd_task, "i2c_cmd", 3072, NULL, 6, NULL);
    xTaskCreate(capture_task, "i2c_snap", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "I2C slave 0x%02x on SDA=%d SCL=%d", I2C_ADDR, PIN_SDA, PIN_SCL);
    return ESP_OK;
}
