#include "http_srv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_mon.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "snap.h"
#include "wifi_mgr.h"

static const char *TAG = "http";

#define DEF_W 640
#define DEF_H 480
#define DEF_Q 50
#define STREAM_BOUNDARY "frame"

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static void get_query(httpd_req_t *req, char *q, size_t len)
{
    if (httpd_req_get_url_query_str(req, q, len) != ESP_OK) q[0] = 0;
}

// ---- /jpg ----

static esp_err_t jpg_handler(httpd_req_t *req)
{
    char q[256], err[160] = "";
    get_query(req, q, sizeof(q));
    snap_params_t p;
    snap_params_default(&p, DEF_W, DEF_H, DEF_Q);
    if (snap_params_parse(q, &p, err, sizeof(err)) != ESP_OK) return send_error(req, "400 Bad Request", err);

    snap_t s;
    if (snap_capture(&p, &s, err, sizeof(err)) != ESP_OK)
        return send_error(req, "500 Internal Server Error", err);
    char hv[32];
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Mode", s.mode);
    snprintf(hv, sizeof(hv), "%dx%d", s.w, s.h);
    httpd_resp_set_hdr(req, "X-Size", hv);
    esp_err_t e = httpd_resp_send(req, (const char *)s.data, s.len);
    snap_free(&s);
    return e;
}

// ---- /jpg_stream（別タスクで配信し、他のリクエストを塞がない）----

typedef struct {
    httpd_req_t *req;
    snap_params_t p;
    int interval_ms;
} stream_ctx_t;

static void stream_task(void *arg)
{
    stream_ctx_t *c = arg;
    char err[160], hdr[128];
    httpd_resp_set_type(c->req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
    httpd_resp_set_hdr(c->req, "Cache-Control", "no-store");
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        snap_t s;
        if (snap_capture(&c->p, &s, err, sizeof(err)) != ESP_OK) {
            ESP_LOGW(TAG, "stream: %s", err);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int hl = snprintf(hdr, sizeof(hdr),
                          "\r\n--" STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                          (unsigned)s.len);
        esp_err_t e = httpd_resp_send_chunk(c->req, hdr, hl);
        if (e == ESP_OK) e = httpd_resp_send_chunk(c->req, (const char *)s.data, s.len);
        snap_free(&s);
        if (e != ESP_OK) break;   // クライアントが切断
        int wait = c->interval_ms - (int)((esp_timer_get_time() - t0) / 1000);
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait));
    }
    httpd_resp_send_chunk(c->req, NULL, 0);
    httpd_req_async_handler_complete(c->req);
    free(c);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    char q[256], err[160] = "", v[12];
    get_query(req, q, sizeof(q));
    stream_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return send_error(req, "500 Internal Server Error", "no memory");
    snap_params_default(&c->p, DEF_W, DEF_H, DEF_Q);
    if (snap_params_parse(q, &c->p, err, sizeof(err)) != ESP_OK) {
        free(c);
        return send_error(req, "400 Bad Request", err);
    }
    int fps = 0;   // 0 = 出せるだけ
    if (httpd_query_key_value(q, "fps", v, sizeof(v)) == ESP_OK) fps = atoi(v);
    c->interval_ms = fps > 0 ? 1000 / fps : 0;
    if (httpd_req_async_handler_begin(req, &c->req) != ESP_OK) {
        free(c);
        return send_error(req, "503 Service Unavailable", "busy");
    }
    if (xTaskCreate(stream_task, "stream", 6144, c, 5, NULL) != pdPASS) {
        httpd_req_async_handler_complete(c->req);
        free(c);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---- /cpu /status ----

static esp_err_t cpu_handler(httpd_req_t *req)
{
    char buf[1536];
    size_t n = cpu_mon_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static void json_str(char *dst, size_t len, const char *s)
{
    size_t o = 0;
    for (; *s && o + 7 < len; s++) {
        if (*s == '"' || *s == '\\') dst[o++] = '\\';
        if ((unsigned char)*s < 0x20) continue;
        dst[o++] = *s;
    }
    dst[o] = 0;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    wifi_sta_status_t st;
    wifi_mgr_status(&st);
    wifi_ap_client_t cl[4];
    int n = wifi_mgr_ap_clients(cl, 4);
    char ap_mac[18], ssid[80], buf[1024];
    wifi_mgr_ap_mac(ap_mac);
    json_str(ssid, sizeof(ssid), st.ssid);
    int o = snprintf(buf, sizeof(buf),
                     "{\"uptime_s\":%lld,\"camera\":%s,\"free_heap\":%u,\"free_psram\":%u,"
                     "\"ap\":{\"ssid\":\"M5Cam\",\"ip\":\"192.168.4.1\",\"mac\":\"%s\",\"clients\":[",
                     (long long)(esp_timer_get_time() / 1000000), snap_ready() ? "true" : "false",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), ap_mac);
    for (int i = 0; i < n; i++)
        o += snprintf(buf + o, sizeof(buf) - o, "%s{\"mac\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}", i ? "," : "",
                      cl[i].mac, cl[i].ip, cl[i].rssi);
    o += snprintf(buf + o, sizeof(buf) - o,
                  "]},\"sta\":{\"connected\":%s,\"ssid\":\"%s\",\"mac\":\"%s\",\"ip\":\"%s\","
                  "\"netmask\":\"%s\",\"gw\":\"%s\",\"rssi\":%d}}",
                  st.connected ? "true" : "false", ssid, st.mac, st.ip, st.netmask, st.gw, st.rssi);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, o);
}

// ---- /wifi  GET:一覧  POST:追加/更新(ssid,pass,prio)  DELETE:削除(ssid) ----

static esp_err_t wifi_list_handler(httpd_req_t *req)
{
    char buf[1024];
    int o = wifi_cfg_list_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, o);
}

static esp_err_t wifi_apply_handler(httpd_req_t *req, bool del)
{
    char q[256];
    get_query(req, q, sizeof(q));
    if (del) strlcat(q, "&del=1", sizeof(q));
    esp_err_t e = wifi_cfg_apply_query(q);
    if (e == ESP_ERR_INVALID_ARG) return send_error(req, "400 Bad Request", "ssid required (max 32) / pass max 64");
    if (e == ESP_ERR_NO_MEM) return send_error(req, "507 Insufficient Storage", "max 8 networks");
    if (e == ESP_ERR_NOT_FOUND) return send_error(req, "404 Not Found", "not found");
    if (e != ESP_OK) return send_error(req, "400 Bad Request", "invalid");
    return wifi_list_handler(req);
}

static esp_err_t wifi_post_handler(httpd_req_t *req) { return wifi_apply_handler(req, false); }
static esp_err_t wifi_delete_handler(httpd_req_t *req) { return wifi_apply_handler(req, true); }

static esp_err_t index_handler(httpd_req_t *req)
{
    static const char html[] =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>M5Cam</title>"
        "<body style='font-family:sans-serif'><h3>M5Cam</h3><ul>"
        "<li><a href='/jpg'>/jpg</a> (w, h, q, rot, gray, fit)</li>"
        "<li><a href='/jpg?w=1600&h=1200&q=100'>/jpg?w=1600&amp;h=1200&amp;q=100</a> (最高画質)</li>"
        "<li><a href='/jpg?w=160&h=120&q=10&gray=1'>/jpg?w=160&amp;h=120&amp;q=10&amp;gray=1</a></li>"
        "<li><a href='/jpg_stream'>/jpg_stream</a> (+ fps)</li>"
        "<li><a href='/cpu'>/cpu</a></li><li><a href='/status'>/status</a></li>"
        "<li><a href='/wifi'>/wifi</a></li></ul>";
    return httpd_resp_send(req, html, sizeof(html) - 1);
}

esp_err_t http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.max_open_sockets = 7;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv;
    esp_err_t e = httpd_start(&srv, &cfg);
    if (e != ESP_OK) return e;
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/jpg", .method = HTTP_GET, .handler = jpg_handler},
        {.uri = "/jpg_stream", .method = HTTP_GET, .handler = stream_handler},
        {.uri = "/cpu", .method = HTTP_GET, .handler = cpu_handler},
        {.uri = "/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/wifi", .method = HTTP_GET, .handler = wifi_list_handler},
        {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post_handler},
        {.uri = "/wifi", .method = HTTP_DELETE, .handler = wifi_delete_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(srv, &uris[i]);
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
