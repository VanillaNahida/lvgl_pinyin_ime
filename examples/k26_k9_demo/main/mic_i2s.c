/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 麦克风采集骨架（示例工程用）。
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │ 这是**桩**：接线与器件型号未知，所以这里只把 I2S 的骨架搭好，引脚和模式由     │
 * │ 下面的宏控制，默认值大概率要在你的板子上改。                                │
 * │                                                                          │
 * │ 组件对麦克风只有四个要求（docs/voice.md §5）：                             │
 * │   16 kHz 或 8 kHz、16 bit、单声道、小端 PCM；与 menuconfig 的采样率一致。   │
 * │ 满足它的方式无所谓 —— PDM 麦克风、I2S codec（ES8311 等）、甚至外挂 MCU 通过  │
 * │ UART 送 PCM 都可以，只要最后能从 mic_i2s_read() 里读出字节。               │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * 默认走 **I2S PDM RX**：ESP32-S3 支持 PDM 接收，裸 MEMS 麦克风（INMP441 之外的
 * PDM 型号，或者 S3 的 PDM 模式）直接接两根线就能用，不需要 codec。
 *
 * 参考板 ESP32_MusicPlayer_V4 上大概率是 I2S codec（ES8311/ES7210 一类），那种情况
 * 见下面 CONFIG_EXAMPLE_MIC_USE_STD_I2S 那一段的说明。
 */

#include "mic_i2s.h"

#include <string.h>

#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "demo_mic";

/* --------------------------------------------------------------- 接线配置 */
/*
 * TODO(1)：按你的板子改这四个宏。
 *
 * 这些值不能瞎猜：写错了的表现是 mic_i2s_read() 一直返回 0（读到静音），或者干脆
 * 初始化报 ESP_ERR_INVALID_ARG。判据很简单 —— 初始化成功后对着麦克风说话，
 * mic_i2s_read() 应该返回一段有明显幅度变化的 PCM；全是 0 就是接错了。
 */
#ifndef CONFIG_EXAMPLE_MIC_I2S_PORT
#define CONFIG_EXAMPLE_MIC_I2S_PORT 0
#endif
/*
 * 端口号直接当 int 用，**不要**写成 (i2s_port_t)：ESP-IDF 6.1 的
 * i2s_chan_config_t.id 就是 int，而 i2s_port_t 这个类型在新版 I2S 驱动里已经没有
 * 了（老代码里的 (i2s_port_t)I2S_NUM_0 这种写法在 6.1 上直接编译不过）。
 * 下面 I2S_CHANNEL_DEFAULT_CONFIG 的第一个参数要的正是这个 id。
 */
#define MIC_I2S_PORT CONFIG_EXAMPLE_MIC_I2S_PORT

/** PDM 时钟脚（对应 MEMS 麦克风的 CLK）。 */
#ifndef CONFIG_EXAMPLE_MIC_PIN_CLK
#define CONFIG_EXAMPLE_MIC_PIN_CLK 41
#endif

/** PDM 数据脚（对应 MEMS 麦克风的 DATA）。 */
#ifndef CONFIG_EXAMPLE_MIC_PIN_DIN
#define CONFIG_EXAMPLE_MIC_PIN_DIN 42
#endif

static i2s_chan_handle_t s_rx;
static bool s_ready;

esp_err_t mic_i2s_init(int sample_rate)
{
    if (s_ready) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    /*
     * DMA 缓冲：40 ms 一帧（16 kHz 单声道 16 bit = 1280 B）。
     * 给 4 个 1 KB 的描述符 = 约 128 ms 的余量，够吸收 LVGL 刷屏时的调度抖动，
     * 又不会让"按下语音键之后说的话"在缓冲里排太久队。
     */
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 512;           /* 512 帧 = 1024 B（16 bit 单声道） */
    chan_cfg.auto_clear = true;             /* 读不到新数据时给 0，而不是旧数据 */

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * TODO(2)：如果参考板用的是 I2S codec（ES8311 + ES7210 这类），把下面这段
     * PDM 配置换成 i2s_std 配置，并另外初始化 codec：
     *
     *   i2s_std_config_t std_cfg = {
     *       .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
     *       .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
     *                                                       I2S_SLOT_MODE_MONO),
     *       .gpio_cfg = { .mclk = ..., .bclk = ..., .ws = ..., .din = ..., .dout = ... },
     *   };
     *   i2s_channel_init_std_mode(s_rx, &std_cfg);
     *
     * 两处要注意：
     *   - mono 槽位下有些 codec 只把信号放在左声道，读到的是半段静音 —— 用
     *     I2S_SLOT_MODE_STEREO 读，再把要的那一路挑出来（见 mic_i2s_read）。
     *   - codec 的采样率要跟着一起设（es8311_sample_frequency_config 之类），
     *     两边不一致会变调，识别率会明显下降。
     */
    const i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = (gpio_num_t)CONFIG_EXAMPLE_MIC_PIN_CLK,
            .din = (gpio_num_t)CONFIG_EXAMPLE_MIC_PIN_DIN,
            .invert_flags = { .clk_inv = false },
        },
    };

    err = i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pdm_rx init failed: %s (pins CLK=%d DIN=%d correct?)",
                 esp_err_to_name(err), CONFIG_EXAMPLE_MIC_PIN_CLK, CONFIG_EXAMPLE_MIC_PIN_DIN);
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "PDM RX ready: %d Hz, CLK=%d DIN=%d", sample_rate,
             CONFIG_EXAMPLE_MIC_PIN_CLK, CONFIG_EXAMPLE_MIC_PIN_DIN);
    return ESP_OK;
}

void mic_i2s_deinit(void)
{
    if (s_rx != NULL) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    s_ready = false;
}

int mic_i2s_read(void *user, void *buf, size_t len, uint32_t timeout_ms)
{
    (void)user;
    if (!s_ready || s_rx == NULL) {
        return -1;
    }

    size_t got = 0;
    const esp_err_t err = i2s_channel_read(s_rx, buf, len, &got, pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_ERR_TIMEOUT) {
        /* 这一拍没赶上：返回 0（组件当静音处理，不报错）。 */
        return 0;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s read failed: %s", esp_err_to_name(err));
        return -1;
    }

    /*
     * TODO(3)：增益与声道。
     *
     * 如果改用立体声槽位（I2S_SLOT_MODE_STEREO）读 codec，缓冲里是左右交错的两个
     * 16 bit 样本，要在这里挑出有信号的那一路、写成连续的单声道：
     *
     *   const int16_t *in = (const int16_t *)buf;
     *   int16_t *out = (int16_t *)buf;
     *   for (size_t i = 0; i < got / 4; i++) {
     *       out[i] = in[i * 2 + MIC_CHANNEL];      // 0 或 1
     *   }
     *   got /= 2;
     *
     * 另外：远端识别对音量不敏感，但太小（贴着嘴说才有一点点波形）会明显掉识别率。
     * 需要软件增益的话在这里乘，注意**别溢出** int16，也注意别把底噪一起放大。
     */
    return (int)got;
}
