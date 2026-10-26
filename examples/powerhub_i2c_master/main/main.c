// M5Stack PowerHub (ESP32-S3) を I2C マスターにして、M5Camera (0x42) から JPEG を取得するサンプル。
// 仕様は docs/i2c.md を参照。PORT.A (SDA=G15, SCL=G16) に Grove ケーブルで接続する。
// 取得した JPEG は base64 でシリアルに出す（decode_jpeg.py で復元）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "master";

#define PIN_SDA   15            // PowerHub PORT.A
#define PIN_SCL   16
#define CAM_ADDR  0x42
#define SCL_HZ    1000000       // 100000 / 400000 / 1000000 で動作確認済み
#define CHUNK     120           // 1 回に読むバイト数（最大 120）
#define WAIT_US   1000          // コマンドを書いてから応答を読むまでの待ち（1ms 以上を推奨）
#define QUERY     "w=1024&h=768&q=50"

static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 0x80) ? (c << 1) ^ 0x07 : c << 1;
    }
    return c;
}

// コマンドを書き、WAIT_US 待ってから応答を読む。応答が不正なら繰り返す。
// 待ちは vTaskDelay では作れない（ティックが 10ms 単位）ので、ビジーウェイトを使う。
static bool transact(i2c_master_dev_handle_t dev, const uint8_t *cmd, size_t cl, uint8_t *resp, size_t rl)
{
    for (int retry = 0; retry < 5; retry++) {
        if (i2c_master_transmit(dev, cmd, cl, 100) == ESP_OK) {
            esp_rom_delay_us(WAIT_US);
            if (i2c_master_receive(dev, resp, rl, 100) == ESP_OK && resp[0] == 0xA5 &&
                crc8(resp + 1, rl - 2) == resp[rl - 1])
                return true;
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
    return false;
}

static void print_b64(const uint8_t *buf, size_t len)
{
    static char line[100];
    printf("JPEG_BEGIN %u\n", (unsigned)len);
    for (size_t off = 0; off < len; off += 57) {
        size_t n = len - off < 57 ? len - off : 57, ol;
        mbedtls_base64_encode((unsigned char *)line, sizeof(line), &ol, buf + off, n);
        line[ol] = 0;
        printf("%s\n", line);
    }
    printf("JPEG_END\n");
}

// 撮影 → 完了待ち → 全データ読み出し。成功すると malloc した JPEG を返す（呼び出し側が free）。
static uint8_t *snap(i2c_master_dev_handle_t dev, uint32_t *len)
{
    uint8_t cmd[1 + 127] = {0x01};
    size_t ql = strlen(QUERY);
    memcpy(cmd + 1, QUERY, ql);
    int64_t t0 = esp_timer_get_time();
    if (i2c_master_transmit(dev, cmd, 1 + ql, 100) != ESP_OK) { ESP_LOGE(TAG, "SNAP write failed"); return NULL; }

    uint8_t st[11];
    int t;
    for (t = 0; t < 500; t++) {                 // 完了待ち（最大 5 秒）
        vTaskDelay(pdMS_TO_TICKS(10));
        if (transact(dev, (uint8_t[]){0x02}, 1, st, 11)) {
            if (st[1] == 2) break;              // 完了
            if (st[1] == 3) { ESP_LOGE(TAG, "camera reports error"); return NULL; }
        }
    }
    if (t == 500) { ESP_LOGE(TAG, "timeout waiting for capture"); return NULL; }
    int64_t t1 = esp_timer_get_time();

    uint32_t total = st[2] | st[3] << 8 | st[4] << 16 | (uint32_t)st[5] << 24;
    if (total == 0 || total > 400000) { ESP_LOGE(TAG, "bad size %u", (unsigned)total); return NULL; }
    uint8_t *buf = malloc(total);
    if (!buf) return NULL;

    for (uint32_t off = 0; off < total;) {
        uint8_t n = total - off < CHUNK ? total - off : CHUNK;
        uint8_t rq[6] = {0x03, off, off >> 8, off >> 16, off >> 24, n};
        uint8_t fr[CHUNK + 3];
        if (!transact(dev, rq, 6, fr, n + 3) || fr[1] != n) {
            ESP_LOGE(TAG, "read failed at %u", (unsigned)off);
            free(buf);
            return NULL;
        }
        memcpy(buf + off, fr + 2, n);
        off += n;
    }
    int64_t t2 = esp_timer_get_time();
    ESP_LOGI(TAG, "%ux%u %u B: capture %d ms, read %d ms (%.1f KB/s)", st[6] | st[7] << 8, st[8] | st[9] << 8,
             (unsigned)total, (int)((t1 - t0) / 1000), (int)((t2 - t1) / 1000), total / 1024.0 / ((t2 - t1) / 1e6));
    *len = total;
    return buf;
}

void app_main(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,       // 1MHz などで不安定なら、外付け 2.2〜4.7kΩ を推奨
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));

    i2c_master_dev_handle_t dev;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = CAM_ADDR, .scl_speed_hz = SCL_HZ };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &dev));

    vTaskDelay(pdMS_TO_TICKS(2000));                // カメラの起動待ち
    for (;;) {
        if (i2c_master_probe(bus, CAM_ADDR, 100) != ESP_OK) {
            ESP_LOGW(TAG, "no device at 0x%02x (SDA=G%d SCL=G%d)", CAM_ADDR, PIN_SDA, PIN_SCL);
        } else {
            uint32_t len;
            uint8_t *jpg = snap(dev, &len);
            if (jpg) { print_b64(jpg, len); free(jpg); }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
