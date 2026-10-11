/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi 连接与 SNTP 对时（示例工程用）。
 *
 * ★ 为什么示例里必须对时：讯飞的鉴权把时间戳签进签名，服务端允许的偏差是 ±300 秒，
 *   超了直接 403（"a valid date or x-date header is required"）。ESP32 没有 RTC 电池，
 *   上电后 time() 是 1970 年，所以**第一次语音识别之前必须完成一次 SNTP 同步**。
 *
 * 组件本身不做这件事：它只检查 time() 是否已经对（早于 2020 年就当没对，返回
 * LV_PINYIN_IME_VOICE_ERR_TIME 并在状态条上提示"系统时间未同步"），把"谁来对时"
 * 留给应用 —— 有些产品用 4G 模组、有些用外部 RTC，不该由输入法组件规定。
 */

#include "net_wifi.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "demo_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

static EventGroupHandle_t s_events;
static bool s_sntp_started;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "connecting to \"%s\" ...", CONFIG_EXAMPLE_WIFI_SSID);
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /*
         * 一直重连：语音输入是"用户按了才用"的功能，网络晚一点通没关系，但不能
         * 悄悄停在断开状态 —— 那时按语音键只会得到"语音连接失败"。
         */
        ESP_LOGW(TAG, "disconnected, retrying");
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t demo_wifi_connect(uint32_t timeout_ms)
{
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s",
             CONFIG_EXAMPLE_WIFI_SSID);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s",
             CONFIG_EXAMPLE_WIFI_PASSWORD);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    const EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                                 pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "no connection within %u ms; voice input will report \"连接失败\"",
                 (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void demo_sntp_start(void)
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;

    /*
     * 用 POLL 模式而不是立即同步：立即同步会阻塞在启动流程里等 DNS + NTP 往返，
     * 而这时 Wi-Fi 可能刚拿到 IP，DNS 还没就绪。POLL 模式下它自己按周期重试，
     * 用户在这期间按语音键就会看到"系统时间未同步" —— 比卡住主流程好。
     */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");   /* 国内网络更稳的备选 */
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (voice input needs the clock within 300 s of UTC)");
}

bool demo_time_synced(void)
{
    /*
     * 与组件里的判据保持一致：time() 早于 2020-09-13 就当没对上。
     * （组件那边用的是 1600000000，这里写成同一个数，改的时候两边一起改。）
     */
    return time(NULL) >= 1600000000;
}
