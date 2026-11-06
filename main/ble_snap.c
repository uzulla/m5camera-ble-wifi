#include "ble_snap.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "snap.h"
#include "status_adv.h"
#include "wifi_mgr.h"

static const char *TAG = "ble";

#define DEV_NAME "M5Cam"   // 広告名は "M5Cam-" + STA MAC 下位 3 バイト（例 M5Cam-445566）。複数台を見分けるため
#define BLE_DEF_W 160
#define BLE_DEF_H 120
#define BLE_DEF_Q 20
#define REQ_MAX 120
#define WIFI_REQ_MAX 255
#define APP_ERR_INVALID 0x80
#define APP_ERR_FULL 0x81
#define APP_ERR_NOT_FOUND 0x82

static const ble_uuid128_t SVC_UUID =
    BLE_UUID128_INIT(0x01, 0x00, 0x5b, 0x8e, 0x1a, 0x7c, 0x2e, 0x9f, 0x6a, 0x4d, 0x4e, 0x0b, 0x00, 0x00, 0x3c, 0x5a);
static const ble_uuid128_t REQ_UUID =
    BLE_UUID128_INIT(0x02, 0x00, 0x5b, 0x8e, 0x1a, 0x7c, 0x2e, 0x9f, 0x6a, 0x4d, 0x4e, 0x0b, 0x00, 0x00, 0x3c, 0x5a);
static const ble_uuid128_t DATA_UUID =
    BLE_UUID128_INIT(0x03, 0x00, 0x5b, 0x8e, 0x1a, 0x7c, 0x2e, 0x9f, 0x6a, 0x4d, 0x4e, 0x0b, 0x00, 0x00, 0x3c, 0x5a);
static const ble_uuid128_t WIFI_UUID =
    BLE_UUID128_INIT(0x04, 0x00, 0x5b, 0x8e, 0x1a, 0x7c, 0x2e, 0x9f, 0x6a, 0x4d, 0x4e, 0x0b, 0x00, 0x00, 0x3c, 0x5a);
static const ble_uuid128_t STATUS_UUID =
    BLE_UUID128_INIT(0x05, 0x00, 0x5b, 0x8e, 0x1a, 0x7c, 0x2e, 0x9f, 0x6a, 0x4d, 0x4e, 0x0b, 0x00, 0x00, 0x3c, 0x5a);
static const ble_uuid128_t CTRL_UUID =
    BLE_UUID128_INIT(0x06, 0x00, 0x5b, 0x8e, 0x1a, 0x7c, 0x2e, 0x9f, 0x6a, 0x4d, 0x4e, 0x0b, 0x00, 0x00, 0x3c, 0x5a);

static uint16_t s_data_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_subscribed;
static uint16_t s_mtu = 23;
static uint8_t s_own_addr_type;
static QueueHandle_t s_q;
static bool s_synced;
static char s_name[16] = DEV_NAME;
static SemaphoreHandle_t s_adv_lock;   // advertise() は host タスクと status_adv タスクの両方から呼ばれる

// scan response: [会社 ID 0xFFFF (LE)][状態 payload ≤27]。AD ヘッダ 2 を足して 31 バイトに収める
#define MFG_MAX 29
static uint8_t s_mfg[MFG_MAX] = {0xFF, 0xFF};
static uint8_t s_mfg_len;

static void advertise(void);

static int req_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    char req[REQ_MAX + 1];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > REQ_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, req, REQ_MAX, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
    req[len] = 0;
    xQueueSend(s_q, req, 0);
    return 0;
}

static int data_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static int wifi_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char buf[512];
        int n = wifi_cfg_list_json(buf, sizeof(buf));
        return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    char q[WIFI_REQ_MAX + 1];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > WIFI_REQ_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, q, WIFI_REQ_MAX, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
    q[len] = 0;
    esp_err_t e = wifi_cfg_apply_query(q);
    ESP_LOGI(TAG, "wifi config via BLE: %s", esp_err_to_name(e));
    if (e == ESP_OK) return 0;
    if (e == ESP_ERR_NO_MEM) return APP_ERR_FULL;
    if (e == ESP_ERR_NOT_FOUND) return APP_ERR_NOT_FOUND;
    return APP_ERR_INVALID;
}

static int status_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    char buf[512];
    int n = status_adv_json(buf, sizeof(buf));
    return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char buf[128];
        int n = wifi_radio_json(buf, sizeof(buf));
        return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    char q[64];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len >= sizeof(q)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, q, sizeof(q) - 1, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
    q[len] = 0;
    bool reboot = false;
    esp_err_t e = wifi_cfg_apply_radio_query(q, &reboot);
    ESP_LOGI(TAG, "ctrl via BLE \"%s\": %s", q, esp_err_to_name(e));
    if (e != ESP_OK) return APP_ERR_INVALID;
    if (reboot) wifi_mgr_reboot_later();
    return 0;
}

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &REQ_UUID.u,
                .access_cb = req_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &DATA_UUID.u,
                .access_cb = data_access,
                .val_handle = &s_data_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &WIFI_UUID.u,
                .access_cb = wifi_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &STATUS_UUID.u,
                .access_cb = status_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &CTRL_UUID.u,
                .access_cb = ctrl_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {0},
        },
    },
    {0},
};

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
            // 転送を速くするため、短い接続間隔を要求する
            struct ble_gap_upd_params up = {
                .itvl_min = 6, .itvl_max = 12, .latency = 0, .supervision_timeout = 400,
            };
            ble_gap_update_params(s_conn, &up);
            // 接続中も状態が見えるように、接続不可（ADV_SCAN_IND）で広告を続ける
            advertise();
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected");
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_mtu = 23;
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_data_handle) s_subscribed = ev->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        ESP_LOGI(TAG, "MTU %u", s_mtu);
        break;
    default: break;
    }
    return 0;
}

static void advertise(void)
{
    xSemaphoreTake(s_adv_lock, portMAX_DELAY);
    ble_gap_adv_stop();   // 広告中なら止める（未広告なら BLE_HS_EALREADY が返るだけ）
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // 名前に ID を入れると 128bit UUID（18 バイト）が 31 バイトに収まらないので、UUID は広告に載せない。
    // クライアントは名前の前方一致 "M5Cam" で探す（GATT のサービス探索は広告と無関係に行える）
    f.name = (uint8_t *)s_name;
    f.name_len = strlen(s_name);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) { ESP_LOGE(TAG, "adv_set_fields %d", rc); xSemaphoreGive(s_adv_lock); return; }
    if (s_mfg_len) {
        struct ble_hs_adv_fields r = {0};
        r.mfg_data = s_mfg;
        r.mfg_data_len = s_mfg_len;
        rc = ble_gap_adv_rsp_set_fields(&r);
        if (rc) ESP_LOGE(TAG, "adv_rsp_set_fields %d", rc);
    }
    bool connected = s_conn != BLE_HS_CONN_HANDLE_NONE;
    struct ble_gap_adv_params ap = {
        .conn_mode = connected ? BLE_GAP_CONN_MODE_NON : BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    if (connected) {
        // GATT の転送を邪魔しないよう、接続中はゆっくり広告する
        ap.itvl_min = BLE_GAP_ADV_ITVL_MS(500);
        ap.itvl_max = BLE_GAP_ADV_ITVL_MS(1000);
    }
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_event, NULL);
    if (rc) ESP_LOGE(TAG, "adv_start %d", rc);
    xSemaphoreGive(s_adv_lock);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    advertise();
    ESP_LOGI(TAG, "advertising as \"%s\"", s_name);
}

void ble_snap_set_status(const uint8_t *payload, size_t len)
{
    if (len > MFG_MAX - 2) len = MFG_MAX - 2;
    memcpy(s_mfg + 2, payload, len);
    s_mfg_len = 2 + len;
    if (s_synced) advertise();
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool notify(const uint8_t *buf, size_t len)
{
    int rc = 0;
    for (int retry = 0; retry < 800; retry++) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
            ESP_LOGW(TAG, "notify: not connected/subscribed");
            return false;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
        if (!om) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        rc = ble_gatts_notify_custom(s_conn, s_data_handle, om);
        if (rc == 0) return true;
        if (rc == BLE_HS_ENOTCONN) break;
        vTaskDelay(pdMS_TO_TICKS(5));   // 送信キューが一杯などは少し待って再送
    }
    ESP_LOGW(TAG, "notify failed rc=%d", rc);
    return false;
}

static void send_error(const char *msg)
{
    uint8_t b[2 + 64];
    b[0] = 0xFF; b[1] = 0xFF;
    size_t n = strlcpy((char *)b + 2, msg, 64);
    notify(b, 2 + n);
}

static void worker(void *arg)
{
    char req[REQ_MAX + 1], err[160];
    uint8_t pkt[256];
    for (;;) {
        xQueueReceive(s_q, req, portMAX_DELAY);
        snap_params_t p;
        snap_params_default(&p, BLE_DEF_W, BLE_DEF_H, BLE_DEF_Q);
        snap_t s;
        if (snap_params_parse(req, &p, err, sizeof(err)) != ESP_OK || snap_capture(&p, &s, err, sizeof(err)) != ESP_OK) {
            send_error(err);
            continue;
        }
        size_t chunk = s_mtu > 3 + 2 ? s_mtu - 3 - 2 : 18;
        if (chunk > sizeof(pkt) - 2) chunk = sizeof(pkt) - 2;
        size_t off = 0;
        uint16_t seq = 0;
        bool ok = true;
        while (ok && (off < s.len || seq == 0)) {
            size_t o = 2;
            pkt[0] = seq & 0xFF; pkt[1] = seq >> 8;
            size_t room = chunk;
            if (seq == 0) {
                uint32_t total = s.len;
                memcpy(pkt + o, &total, 4);
                o += 4;
                room -= 4;
            }
            size_t n = s.len - off < room ? s.len - off : room;
            memcpy(pkt + o, s.data + off, n);
            ok = notify(pkt, o + n);
            off += n;
            seq++;
        }
        ESP_LOGI(TAG, "sent %u bytes (%dx%d, %s) in %u packets%s", (unsigned)s.len, s.w, s.h, s.mode, seq,
                 ok ? "" : " [aborted]");
        snap_free(&s);
    }
}

esp_err_t ble_snap_start(void)
{
    s_q = xQueueCreate(2, REQ_MAX + 1);
    s_adv_lock = xSemaphoreCreateMutex();
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) return e;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_att_set_preferred_mtu(247);
    int rc = ble_gatts_count_cfg(GATT_SVCS);
    if (!rc) rc = ble_gatts_add_svcs(GATT_SVCS);
    if (rc) return ESP_FAIL;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_name, sizeof(s_name), DEV_NAME "-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ble_svc_gap_device_name_set(s_name);
    xTaskCreate(worker, "ble_snap", 6144, NULL, 4, NULL);
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
