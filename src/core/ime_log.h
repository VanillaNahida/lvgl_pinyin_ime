/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Logging for the LVGL-independent core: ESP_LOG on the target, stderr on the
 * host so that test_apps/host_core_test can compile the very same sources.
 */

#pragma once

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#define IME_LOGI(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define IME_LOGW(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
#define IME_LOGE(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#else
#include <stdio.h>
#define IME_LOGI(tag, ...)                       \
    do {                                         \
        printf("[%s] ", (tag));                  \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
    } while (0)
#define IME_LOGW(tag, ...) IME_LOGI(tag, __VA_ARGS__)
#define IME_LOGE(tag, ...) IME_LOGI(tag, __VA_ARGS__)
#endif
