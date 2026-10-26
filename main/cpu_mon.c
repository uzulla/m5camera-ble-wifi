#include "cpu_mon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MAX_TASKS 40
#define PERIOD_MS 1000

typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    float pct;   // 1 コア分を 100% としたときの使用率
} task_pct_t;

static SemaphoreHandle_t s_lock;
static float s_core[2];
static int s_ncore = 2;
static task_pct_t s_tasks[MAX_TASKS];
static int s_ntasks;
static int64_t s_window_us;

typedef struct { TaskHandle_t h; uint32_t rt; } prev_t;

static void sampler(void *arg)
{
    TaskStatus_t *cur = malloc(sizeof(TaskStatus_t) * MAX_TASKS);
    prev_t prev[MAX_TASKS];
    int nprev = 0;
    uint32_t prev_total = 0;
    int64_t prev_us = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
        uint32_t total;
        int n = uxTaskGetSystemState(cur, MAX_TASKS, &total);
        int64_t now = esp_timer_get_time();
        uint32_t dtotal = total - prev_total;
        if (dtotal == 0) continue;

        task_pct_t tasks[MAX_TASKS];
        int nt = 0;
        float core[2] = {0, 0};
        for (int i = 0; i < n; i++) {
            uint32_t before = 0;
            for (int j = 0; j < nprev; j++)
                if (prev[j].h == cur[i].xHandle) { before = prev[j].rt; break; }
            float pct = (float)(cur[i].ulRunTimeCounter - before) * 100.0f / dtotal;
            if (!strcmp(cur[i].pcTaskName, "IDLE0")) core[0] = 100.0f - pct;
            else if (!strcmp(cur[i].pcTaskName, "IDLE1")) core[1] = 100.0f - pct;
            else if (nt < MAX_TASKS) {
                strlcpy(tasks[nt].name, cur[i].pcTaskName, sizeof(tasks[nt].name));
                tasks[nt].pct = pct;
                nt++;
            }
        }
        nprev = 0;
        for (int i = 0; i < n && i < MAX_TASKS; i++) {
            prev[nprev].h = cur[i].xHandle;
            prev[nprev].rt = cur[i].ulRunTimeCounter;
            nprev++;
        }
        prev_total = total;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_core[0] = core[0] < 0 ? 0 : core[0];
        s_core[1] = core[1] < 0 ? 0 : core[1];
        memcpy(s_tasks, tasks, sizeof(task_pct_t) * nt);
        s_ntasks = nt;
        s_window_us = now - prev_us;
        prev_us = now;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t cpu_mon_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    return xTaskCreate(sampler, "cpu_mon", 4096, NULL, 1, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}

size_t cpu_mon_json(char *buf, size_t len)
{
    size_t o = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    float total = (s_core[0] + s_core[1]) / s_ncore;
    o += snprintf(buf + o, len - o,
                  "{\"total\":%.1f,\"core0\":%.1f,\"core1\":%.1f,\"window_ms\":%lld,"
                  "\"free_heap\":%u,\"free_psram\":%u,\"tasks\":[",
                  total, s_core[0], s_core[1], (long long)(s_window_us / 1000),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    bool first = true;
    for (int i = 0; i < s_ntasks && o < len; i++) {
        if (s_tasks[i].pct < 0.05f) continue;
        o += snprintf(buf + o, len - o, "%s{\"name\":\"%s\",\"cpu\":%.1f}", first ? "" : ",",
                      s_tasks[i].name, s_tasks[i].pct);
        first = false;
    }
    if (o < len) o += snprintf(buf + o, len - o, "]}");
    xSemaphoreGive(s_lock);
    return o < len ? o : len - 1;
}
