#pragma once
#include <stddef.h>
#include "esp_err.h"

esp_err_t cpu_mon_start(void);
// 直近の 1 秒間の CPU 使用率を JSON で buf に書く。書いた長さを返す
size_t cpu_mon_json(char *buf, size_t len);
