/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音听写会话的状态机与任务。
 *
 * 线程模型（这里是整个功能里最容易出错的地方，写清楚）：
 *
 *   ┌─ LVGL 线程（app_main 里跑 lv_timer_handler）────────────────────────┐
 *   │  调 start/stop/cancel、收状态回调、画界面                            │
 *   │  读 state()/text()/error()：只是读结构体字段，加锁后读             │
 *   └───────────────▲──────────────────────────────┬─────────────────────┘
 *                   │ 状态/结果回调（经 lv_async_call 转过来）           │
 *                   │                                                │ start/stop/cancel
 *   ┌───────────────┴──────────────────────────────▼─────────────────────┐
 *   │  组件任务（voice_task）                                            │
 *   │   · 每 40 ms 调 host->audio_read() 拿 PCM，拼帧，host->ws_send_text │
 *   │   · 从队列里取 host 塞进来的返回帧，解析、修正段落表、推进状态      │
 *   └───────────────▲──────────────────────────────┬─────────────────────┘
 *                   │ 返回帧入队                     │ ws_open / ws_send_text / ws_close
 *   ┌───────────────┴──────────────────────────────▼─────────────────────┐
 *   │  调用方的网络任务（esp_websocket_client 等）                        │
 *   │  收到文本帧 → lv_pinyin_ime_voice_ws_message()（只入队，不碰 LVGL） │
 *   └────────────────────────────────────────────────────────────────────┘
 *
 * 为什么 ws_message 只入队：它跑在调用方的网络任务里，那里**绝对不能碰 LVGL**。
 * 而返回帧要在 LVGL 线程里上屏、在组件任务里推进状态机，所以统一走队列。
 *
 * 为什么把 40 ms 节拍放在组件而不是调用方：文档明确要求"每次发送音频间隔 40 ms、
 * 字节数为 1280 的整数倍"。让每个调用方自己实现这个节拍，等于把一条协议要求散到
 * 所有接入方；放在这里，调用方的 audio_read 只要"阻塞读 I2S"就行。
 */

#include "voice/ime_voice_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "core/ime_log.h"
#include "voice/ime_voice_proto.h"

static const char *TAG = "ime_voice";

/* ------------------------------------------------------------------ 宏 */

#ifndef CONFIG_LV_PINYIN_IME_VOICE_ENABLE
#define CONFIG_LV_PINYIN_IME_VOICE_ENABLE 0
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_APPID
#define CONFIG_LV_PINYIN_IME_VOICE_APPID ""
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_APIKEY
#define CONFIG_LV_PINYIN_IME_VOICE_APIKEY ""
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_APISECRET
#define CONFIG_LV_PINYIN_IME_VOICE_APISECRET ""
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_ENDPOINT
#define CONFIG_LV_PINYIN_IME_VOICE_ENDPOINT "wss://iat-api.xfyun.cn/v2/iat"
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_LANGUAGE
#define CONFIG_LV_PINYIN_IME_VOICE_LANGUAGE "zh_cn"
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_DOMAIN
#define CONFIG_LV_PINYIN_IME_VOICE_DOMAIN "iat"
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_ACCENT
#define CONFIG_LV_PINYIN_IME_VOICE_ACCENT "mandarin"
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_SAMPLE_RATE
#define CONFIG_LV_PINYIN_IME_VOICE_SAMPLE_RATE 16000
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_EOS_MS
#define CONFIG_LV_PINYIN_IME_VOICE_EOS_MS 2000
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_DWA
#define CONFIG_LV_PINYIN_IME_VOICE_DWA 1
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_PUNCTUATION
#define CONFIG_LV_PINYIN_IME_VOICE_PUNCTUATION 1
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_LIVE_PREVIEW
#define CONFIG_LV_PINYIN_IME_VOICE_LIVE_PREVIEW 1
#endif
#ifndef CONFIG_LV_PINYIN_IME_VOICE_COMMIT_REPLACE
#define CONFIG_LV_PINYIN_IME_VOICE_COMMIT_REPLACE 0
#endif

/*
 * 会话时长上限。
 *
 * 文档：整个会话最多 60 s，超过会报 10114/10014。取 55 s 主动收尾，留 5 s 给最后
 * 一块结果的往返 —— 与其让用户撞上"识别突然失败"，不如我们先把结果拿到手。
 */
#define IME_VOICE_MAX_SESSION_MS 55000u

/** 发出结束帧后等最终结果的时长上限；超时就按已有文本收尾，不再干等。 */
#define IME_VOICE_FINISH_TIMEOUT_MS 6000u

/** 一帧音频的节拍（文档建议值）。 */
#define IME_VOICE_TICK_MS IME_VOICE_FRAME_MS

/**
 * 语音任务的栈深度。
 *
 * ★ 单位是**字节**，不是字。ESP-IDF 的 FreeRTOS 端口里
 *   `portSTACK_TYPE` 就是 `uint8_t`（见 FreeRTOS-Kernel/portable/riscv 与
 *   /portable/xtensa 的 portmacro.h），所以 xTaskCreate 的这个参数直接就是字节数。
 *
 *   这里踩过坑：曾经按"字"理解写成 1536，以为等于 6 KB，实际只给了 1536 字节，
 *   而 `ime_voice_build_auth_url()` 一个函数就要 2.3 KB 局部数组 —— 一按语音键
 *   立刻 Stack protection fault：
 *
 *     Detected in task "ime_voice" at ime_voice_build_auth_url
 *     Stack bounds: 0x2f04310c - 0x2f043700   ← 1524 字节，就是这个值
 *     Stack pointer: 0x2f042df0               ← 越界 796 字节
 *
 * 现在的大缓冲全都在会话工作区（堆）上，栈上最大的几条链实测如下
 * （-fstack-usage，tools/check_stack_usage.ps1）：
 *
 *   voice_task                                   112 B
 *   └ build_auth_url_now 80 → build_auth_url 96 → hmac_sha256 368 → sha256_block 352
 *                                                合计 ≈ 1008 B   ← 最坏
 *   └ handle_reply 112 → parse_reply 80 → parse_result 144 → parse_seg 96
 *                                                合计 ≈   560 B
 *
 * build_auth_url 曾经是 2464 B（一串局部数组），**一按下语音键就崩** —— 那是这个
 * 8192 的由来，也是为什么那些缓冲现在在堆上。
 *
 * 8192 字节相对最坏链约 1 KB 有 8 倍余量，足够吸收 IDF/FreeRTOS/日志这些不在我们
 * 统计里的调用。**调小之前先跑 tools/check_stack_usage.ps1。**
 */
#define IME_VOICE_TASK_STACK 8192

/* b64：131 K 是 16k 采样率下 5.5 秒的音频，够用且不会让帧长得离谱。 */
#define IME_VOICE_FRAME_MAX_B64 (IME_VOICE_FRAME_BYTES_16K * 2 + 64)
#define IME_VOICE_FIRST_FRAME_MAX (IME_VOICE_FIRST_FRAME_OVERHEAD + IME_VOICE_FRAME_MAX_B64 + 64)

/** 收到但还没处理的返回帧缓冲（每条一帧，约几百字节到 2 KB）。 */
#define IME_VOICE_MAX_FRAME 4096

/* ------------------------------------------------------------------ 类型 */

typedef struct {
    char  *json;                /* 堆上，处理完 free */
    size_t len;
} voice_msg_t;

/**
 * 会话的临时工作区：堆上，随会话分配一次，任务循环反复复用。
 *
 * ★ 这些原来都是**任务里的局部变量**，那是真机上崩溃的直接原因：
 *
 *     ime_voice_reply_t 一个就 12 744 B（segs[32] × 392 B），而语音任务的栈当时
 *     只有 4096 B。ESP-IDF 建任务时按 4096 字（16 KB）算，扣掉内核与日志开销剩
 *     8 KB —— 仍然装不下 12.7 KB 的局部变量，于是**第一帧返回**就砸了：
 *
 *       Guru Meditation Error: Core 0 panic'ed (Stack protection fault)
 *       Detected in task "ime_voice" at handle_reply ... ime_voice.c:344
 *       Stack bounds: 0x2f04310c - 0x2f045100   SP: 0x2f041e50
 *
 *   修法不是"把栈调大"：12.7 KB 的局部变量本身就不该出现在任何任务栈上（LVGL
 *   所在的主任务也才 12 KB）。搬到堆上之后，栈占用回到几十字节，任务栈小一点都行。
 *
 * 访问约定：只在**组件任务**里用（handle_reply / voice_task 都在该任务内），
 * 所以不需要加锁。唯一例外是 before_text，见 ime_voice_transcript_apply()。
 */
typedef struct {
    /** 解析后的返回帧：最大的一块，也是崩溃的元凶。 */
    ime_voice_reply_t reply;
    /** 动态修正前后的整句快照，用来判断"界面要不要重画"。 */
    char before_text[IME_VOICE_TRANSCRIPT_MAX];
    /** 拼下一帧 JSON 用的缓冲（首帧比后续帧大：多带 common + business）。 */
    char frame[IME_VOICE_FIRST_FRAME_MAX];
    /** 从麦克风读一帧 PCM 的缓冲。 */
    uint8_t pcm[IME_VOICE_FRAME_BYTES_16K];
    /**
     * 拼鉴权 URL 的中间缓冲（约 2.3 KB）。
     *
     * ★ 它也必须在堆上：原来这些是 ime_voice_build_auth_url() 里的局部数组，真机上
     *   一进函数就栈溢出 —— 那个任务的栈当时只有 1536 **字节**，而函数要 2.3 KB。
     *   详见头文件里 ime_voice_auth_scratch_t 的注释。
     */
    ime_voice_auth_scratch_t auth;
} ime_voice_scratch_t;

struct lv_pinyin_ime_voice {
    lv_pinyin_ime_voice_cfg_t cfg;
    ime_voice_params_t        params;    /* 由 cfg 翻译而来，供协议层用 */

    SemaphoreHandle_t lock;
    TaskHandle_t      task;
    bool              task_running;

    volatile lv_pinyin_ime_voice_state_t state;
    volatile lv_pinyin_ime_voice_err_t   err;
    volatile int                         server_code;
    volatile bool                        stop_requested;
    volatile bool                        cancel_requested;
    volatile bool                        ws_open_done;
    volatile bool                        ws_closed_by_peer;

    /* 待处理的返回帧队列（生产者：网络任务；消费者：组件任务） */
    voice_msg_t msgs[IME_VOICE_QUEUE_LEN];
    volatile size_t msg_head;
    volatile size_t msg_tail;
    volatile size_t msg_dropped;

    /* 识别文本 */
    ime_voice_transcript_t transcript;
    char                   text[IME_VOICE_TRANSCRIPT_MAX];
    bool                   result_ready;
    uint32_t               audio_ms;

    /* 上一帧是否失败（连续失败才报错，偶发一次不打断会话） */
    int consecutive_audio_errors;

    /* 音频电平判定：本次会话里峰值低于静音阈值的帧数。 */
    uint32_t silent_frames;
    /** 本次会话里最大的那一帧峰值（用来给"为什么没识别出来"下结论）。 */
    uint32_t peak_max;

    /* 回调 */
    lv_pinyin_ime_voice_cb_t state_cb;
    void                    *state_cb_user;
    lv_pinyin_ime_voice_cb_t result_cb;
    void                    *result_cb_user;

    /** 临时工作区：堆上，见上面的说明。 */
    ime_voice_scratch_t scratch;
};

/*
 * ★ 编译期护栏：不许再把大块缓冲放回任务栈。
 *
 * 这个坑踩过三次（reply 12.7 KB、auth scratch 2.3 KB、还有一次把栈深度按"字"当
 * "字节"用），每次都是真机 Stack protection fault。编译器不会为大局部变量报警，
 * 所以用断言把关键事实钉住：
 *
 *   1. 工作区里必须有 reply 和 auth 两块缓冲（谁把它们挪回局部变量，结构体就会
 *      变小，断言立刻失败）；
 *   2. 任务栈深度（字节）不能低于 4 KB —— 防止再有人按"字"把它写成一千多。
 */
_Static_assert(sizeof(((ime_voice_scratch_t *)0)->reply) == sizeof(ime_voice_reply_t),
               "scratch.reply must stay in the heap-side workspace");
_Static_assert(sizeof(ime_voice_scratch_t) >=
                   sizeof(ime_voice_reply_t) + sizeof(ime_voice_auth_scratch_t),
               "scratch must hold both the reply and the auth buffer (heap-side)");
_Static_assert(IME_VOICE_TASK_STACK >= 4096,
               "the voice task stack is a BYTE count (StackType_t is uint8_t in the "
               "ESP-IDF ports); do not shrink it below 4 KB");

/* ------------------------------------------------------------ 小工具 */

/**
 * 整数平方根（向下取整）。
 *
 * 为什么不用 __builtin_sqrt：那会引入浮点指令与 libm 的一小段代码，在 MCU 上既
 * 费 flash 又有自己的栈开销 —— 而这个函数只是给日志算一个 RMS 量级，用整数足够。
 */
static uint32_t isqrt_u32(uint64_t x)
{
    if (x == 0) {
        return 0;
    }
    uint64_t r = x;
    uint64_t r1 = (r + 1u) / 2u;
    /* 牛顿迭代，收敛很快；最多几轮就到不动点。 */
    while (r1 < r) {
        r = r1;
        r1 = (r + x / r) / 2u;
    }
    return (uint32_t)r;
}

/**
 * 把"最高位以下那一段"换算成 dB（×10，即分贝的十分位）。
 *
 * 表里是 `10 * 20 * log10(1 + i/16)`，i = 0..15，也就是把每两个二进制位之间
 * 线性插值成 16 档。**预计算成整数表**，运行时不做任何浮点运算
 * （上一版这里写了 `* 6.0206f`，在 MCU 上会白拉进浮点支持，属于自己反对过的做法）。
 */
static const int16_t s_db_tenths[16] = {
    0, 5, 10, 15, 20, 24, 29, 33, 38, 42, 46, 50, 54, 58, 62, 66
};

/**
 * 16 bit 满量程幅值 → dBFS（0 = 满量程，负得越多越安静）。
 *
 * 为什么要换算成 dB：光看 `peak=234 rms=15` 很难判断"这算不算小"。换算之后一眼
 * 就看出来：
 *
 *   peak=234  → 约 -43 dBFS     ← 比正常说话低 20~35 dB，VAD 根本不会触发
 *   peak=3277 → 约 -20 dBFS     ← 正常说话的下限
 *   peak=16384→ 约 -6  dBFS     ← 大声说话
 *
 * 这正是"说了话却什么都没识别出来"的量化判据。
 */
static int dbfs_from_amplitude(uint32_t amp)
{
    if (amp == 0) {
        return -120;                    /* 完全静音，给个下限 */
    }
    int bits = 0;
    for (uint32_t v = amp >> 1; v != 0; v >>= 1) {
        bits++;                         /* bits = floor(log2(amp)) */
    }
    const uint32_t base = 1u << bits;   /* amp / base 落在 [1,2) */
    const int idx = (int)(((amp - base) * 16u) / base);
    /* 每高一位 = 6.02 dB = 60.2 个十分位；满量程 15 位对应 90.3 dB */
    const int tenths = bits * 602 / 10 + s_db_tenths[(idx < 16) ? idx : 15];
    return (tenths - 903) / 10;
}

static void lock(lv_pinyin_ime_voice_t *v)
{
    if (v->lock != NULL) {
        xSemaphoreTake(v->lock, portMAX_DELAY);
    }
}

static void unlock(lv_pinyin_ime_voice_t *v)
{
    if (v->lock != NULL) {
        xSemaphoreGive(v->lock);
    }
}

/** 由 cfg 翻译出协议层参数。 */
static void cfg_to_params(const lv_pinyin_ime_voice_cfg_t *cfg, ime_voice_params_t *p)
{
    ime_voice_params_defaults(p);
    if (cfg->appid[0] != '\0') {
        snprintf(p->appid, sizeof(p->appid), "%s", cfg->appid);
    }
    if (cfg->apikey[0] != '\0') {
        snprintf(p->apikey, sizeof(p->apikey), "%s", cfg->apikey);
    }
    if (cfg->apisecret[0] != '\0') {
        snprintf(p->apisecret, sizeof(p->apisecret), "%s", cfg->apisecret);
    }
    if (cfg->endpoint[0] != '\0') {
        snprintf(p->endpoint, sizeof(p->endpoint), "%s", cfg->endpoint);
    }
    if (cfg->language[0] != '\0') {
        snprintf(p->language, sizeof(p->language), "%s", cfg->language);
    }
    if (cfg->domain[0] != '\0') {
        snprintf(p->domain, sizeof(p->domain), "%s", cfg->domain);
    }
    if (cfg->accent[0] != '\0') {
        snprintf(p->accent, sizeof(p->accent), "%s", cfg->accent);
    }
    p->rate = (cfg->sample_rate == 8000) ? IME_VOICE_RATE_8K : IME_VOICE_RATE_16K;
    p->dwa = cfg->dynamic_correction;
    p->ptt = cfg->punctuation;
    p->eos_ms = (cfg->eos_ms > 0) ? cfg->eos_ms : 2000;
}

/* ------------------------------------------------------------ 状态与上报 */

static void set_state(lv_pinyin_ime_voice_t *v, lv_pinyin_ime_voice_state_t st)
{
    if (v->state == st) {
        return;
    }
    v->state = st;
    if (v->state_cb != NULL) {
        /* 回调在组件任务里；UI 那边要自己转到 LVGL 线程（见 ime_voice_ui.c）。 */
        v->state_cb(v, v->state_cb_user);
    }
}

/** 按当前错误码更新可读文案（存到 text 之外的静态区，见 state_text）。 */
static const char *err_text(lv_pinyin_ime_voice_err_t e)
{
    switch (e) {
    case LV_PINYIN_IME_VOICE_ERR_NOT_CONFIGURED:
        return "语音未配置密钥";
    case LV_PINYIN_IME_VOICE_ERR_NO_HOST:
        return "语音未接网络接口";
    case LV_PINYIN_IME_VOICE_ERR_TIME:
        return "系统时间未同步";
    case LV_PINYIN_IME_VOICE_ERR_WS_OPEN:
        return "语音连接失败";
    case LV_PINYIN_IME_VOICE_ERR_WS_SEND:
        return "语音发送失败";
    case LV_PINYIN_IME_VOICE_ERR_AUDIO:
        return "麦克风读取失败";
    case LV_PINYIN_IME_VOICE_ERR_WS_CLOSED:
        return "语音连接已断开";
    case LV_PINYIN_IME_VOICE_ERR_TIMEOUT:
        return "语音识别超时";
    case LV_PINYIN_IME_VOICE_ERR_SERVER:
        return "语音服务返回错误";
    case LV_PINYIN_IME_VOICE_ERR_BUSY:
        return "语音正在识别";
    default:
        return NULL;
    }
}

/**
 * 收尾：关连接、把已识别的文本落地、状态回到 IDLE。
 *
 * ★ 必须由组件任务调用（会 join 自己、会动 transcript）。
 */
static void finish_session(lv_pinyin_ime_voice_t *v, lv_pinyin_ime_voice_err_t err,
                           int server_code)
{
    if (v->cfg.host != NULL && v->cfg.host->ws_close != NULL) {
        v->cfg.host->ws_close(v->cfg.host->user);
    }

    lock(v);
    v->err = err;
    v->server_code = server_code;
    /* 取消的话连已经收到的部分也丢掉（用户按的是"取消"）。 */
    if (v->cancel_requested) {
        memset(v->transcript.parts, 0, sizeof(v->transcript.parts));
        v->transcript.count = 0;
        v->text[0] = '\0';
        v->result_ready = false;
    } else {
        ime_voice_transcript_text(&v->transcript, v->text, sizeof(v->text));
        v->result_ready = (v->text[0] != '\0');
    }
    unlock(v);

    set_state(v, (err == LV_PINYIN_IME_VOICE_ERR_NONE) ? LV_PINYIN_IME_VOICE_DONE
                                                      : LV_PINYIN_IME_VOICE_ERROR);

    if (v->result_cb != NULL && v->result_ready) {
        v->result_cb(v, v->result_cb_user);
    }

    IME_LOGI(TAG, "session finished: err=%d server=%d text=\"%s\" audio=%ums",
             (int)err, server_code, v->text, (unsigned)v->audio_ms);

    /*
     * 结果是空的时候，把"为什么空"直接说清楚 —— 连同实测电平一起给。
     *
     * 这是被真机逼出来的诊断：服务端拿到静音音频时会**正常**返回一个空结果
     * （code=0、status=2、ws=[]），组件于是报 err=0、text=""，光看这行日志完全
     * 看不出问题在麦克风侧。实测的那次就是 peak=234（约 -43 dBFS）、rms=15，
     * 比正常说话低 20~35 dB，服务端 VAD 自然不触发。
     */
    if (v->text[0] == '\0' && v->audio_ms > 0) {
        const uint32_t frames = v->audio_ms / IME_VOICE_FRAME_MS;
        const int peak_db = dbfs_from_amplitude(v->peak_max);
        if (v->silent_frames >= frames && frames > 0) {
            IME_LOGW(TAG, "empty result: all %u frames were below -40 dBFS "
                          "(session peak %u = %d dBFS) -- the microphone captured no "
                          "speech. Check, in this order: (1) channel selection -- the "
                          "codec runs 2ch, the mono downmix may be taking the silent "
                          "one; (2) codec ADC gain; (3) mic routing/power",
                     (unsigned)frames, (unsigned)v->peak_max, peak_db);
        } else if (peak_db < -30) {
            IME_LOGW(TAG, "empty result: audio peaked at only %d dBFS (peak %u); speech "
                          "should reach -20..-6 dBFS -- add gain, the server's VAD never "
                          "triggered",
                     peak_db, (unsigned)v->peak_max);
        } else {
            IME_LOGW(TAG, "empty result but audio peaked at %d dBFS (peak %u) -- the "
                          "level is fine, so suspect the sample rate or PCM format "
                          "(e.g. resampling 48k->16k by the wrong factor plays back "
                          "slow/fast and the server hears nonsense)",
                     peak_db, (unsigned)v->peak_max);
        }
    } else if (v->peak_max > 0) {
        /* 有结果时也报一下电平：识别"漏字"往往就是电平偏低导致的。 */
        const int peak_db = dbfs_from_amplitude(v->peak_max);
        if (peak_db < -25) {
            IME_LOGW(TAG, "recognised \"%s\" but audio only peaked at %d dBFS -- a low "
                          "level like this drops characters; consider more mic gain",
                     v->text, peak_db);
        } else {
            IME_LOGI(TAG, "audio peak was %d dBFS (healthy range is -20..-6)",
                     peak_db);
        }
    }
}

/* ------------------------------------------------------------ 消息队列 */

static void queue_push(lv_pinyin_ime_voice_t *v, const char *json, size_t len)
{
    if (json == NULL || len == 0 || len > IME_VOICE_MAX_FRAME) {
        return;
    }
    lock(v);
    const size_t next = (v->msg_tail + 1u) % IME_VOICE_QUEUE_LEN;
    if (next == v->msg_head) {
        /* 队列满：丢最旧的一条，保新的（新的更接近当前状态）。 */
        char *old = v->msgs[v->msg_head].json;
        v->msg_head = (v->msg_head + 1u) % IME_VOICE_QUEUE_LEN;
        free(old);
        v->msg_dropped++;
    }
    /* 多要一个字节：解析是就地做的，会写 buf[len]。 */
    char *copy = (char *)malloc(len + 1u);
    if (copy == NULL) {
        unlock(v);
        return;
    }
    memcpy(copy, json, len);
    copy[len] = '\0';
    v->msgs[v->msg_tail].json = copy;
    v->msgs[v->msg_tail].len = len;
    v->msg_tail = next;
    unlock(v);
}

/** 取一条待处理的消息（所有权转移给调用者）。 */
static char *queue_pop(lv_pinyin_ime_voice_t *v, size_t *len)
{
    char *json = NULL;
    lock(v);
    if (v->msg_head != v->msg_tail) {
        json = v->msgs[v->msg_head].json;
        *len = v->msgs[v->msg_head].len;
        v->msgs[v->msg_head].json = NULL;
        v->msg_head = (v->msg_head + 1u) % IME_VOICE_QUEUE_LEN;
    }
    unlock(v);
    return json;
}

/* ------------------------------------------------------------ 返回帧处理 */

/**
 * 处理一条返回帧。返回 true 表示会话该结束了。
 *
 * @param final_out 服务端说这是最后一块
 */
static bool handle_reply(lv_pinyin_ime_voice_t *v, char *json, size_t len, bool *final_out)
{
    /* ★ 12.7 KB 的结构体，必须放在会话的工作区（堆）上 —— 见 ime_voice_scratch_t。 */
    ime_voice_reply_t *r = &v->scratch.reply;
    *final_out = false;

    if (!ime_voice_parse_reply(json, len, r)) {
        IME_LOGW(TAG, "unparsable reply (%u bytes), ignored", (unsigned)len);
        return false;
    }

    if (r->code != 0) {
        IME_LOGE(TAG, "server error code=%d message=\"%s\"", r->code, r->message);
        lock(v);
        v->server_code = r->code;
        unlock(v);
        return true;                    /* 服务端报错：立刻收尾 */
    }

    if (!r->has_result) {
        return false;                   /* 握手后的空帧之类，忽略 */
    }

    lock(v);
    const bool changed = ime_voice_transcript_apply(&v->transcript, r,
                                                    v->scratch.before_text);
    if (changed) {
        ime_voice_transcript_text(&v->transcript, v->text, sizeof(v->text));
    }
    /* data.status == 2 或 result.ls == true 都表示"最后一块"。 */
    const bool last = (r->status == 2) || r->ls;
    unlock(v);

    if (last) {
        *final_out = true;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ 组件任务 */

static void voice_task(void *arg)
{
    lv_pinyin_ime_voice_t *v = (lv_pinyin_ime_voice_t *)arg;
    const lv_pinyin_ime_voice_host_t *host = v->cfg.host;
    /* 帧与 PCM 缓冲都在会话的工作区（堆）上：它们是 3.9 KB，放栈上会把这 4 KB 的
     * 任务栈吃掉一大半。用局部别名只是为了下面几行好读。 */
    char    *frame = v->scratch.frame;
    uint8_t *pcm = v->scratch.pcm;
    const size_t frame_cap = sizeof(v->scratch.frame);

    /* --- 1. 鉴权 + 连接 --- */
    int64_t now = (int64_t)time(NULL);
    if (now < 1600000000) {             /* 2020-09 之前 = 时钟没对上 */
        IME_LOGE(TAG, "system clock is not set (%lld); run SNTP before voice input",
                 (long long)now);
        finish_session(v, LV_PINYIN_IME_VOICE_ERR_TIME, 0);
        goto done;
    }

    if (!ime_voice_build_auth_url_now(&v->params, now, &v->scratch.auth, frame, frame_cap)) {
        IME_LOGE(TAG, "cannot build the auth url");
        finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_OPEN, 0);
        goto done;
    }

    IME_LOGI(TAG, "connecting: %s", v->params.endpoint);
    if (host->ws_open(host->user, frame) != 0) {
        IME_LOGE(TAG, "ws_open failed");
        finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_OPEN, 0);
        goto done;
    }
    v->ws_open_done = true;
    set_state(v, LV_PINYIN_IME_VOICE_LISTENING);

    /* --- 2. 推音频 + 收结果 --- */
    const size_t frame_bytes = ime_voice_frame_bytes(v->params.rate);
    uint32_t elapsed_ms = 0;
    uint32_t finish_deadline_ms = 0;
    uint32_t frame_index = 0;           /* 已发送的音频帧序号（诊断用） */
    bool     sent_last = false;
    bool     server_final = false;
    bool     forced_stop = false;

    while (true) {
        const uint32_t t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* 2a. 先把队列里的返回帧处理掉 */
        size_t mlen = 0;
        char *msg = NULL;
        while ((msg = queue_pop(v, &mlen)) != NULL) {
            bool last = false;
            const bool stop_now = handle_reply(v, msg, mlen, &last);
            free(msg);
            if (last) {
                server_final = true;
            }
            if (stop_now) {
                if (v->server_code != 0) {
                    finish_session(v, LV_PINYIN_IME_VOICE_ERR_SERVER, v->server_code);
                    goto done;
                }
                /* 服务端主动结束（正常或 status==2）：收尾 */
                if (!sent_last) {
                    const size_t n = ime_voice_build_last_frame(frame, frame_cap);
                    if (n > 0) {
                        (void)host->ws_send_text(host->user, frame);
                    }
                    sent_last = true;
                }
                finish_session(v, LV_PINYIN_IME_VOICE_ERR_NONE, 0);
                goto done;
            }
        }

        /* 2b. 对端断了：如果已经拿到最终结果就当正常结束，否则报错 */
        if (v->ws_closed_by_peer) {
            if (server_final) {
                finish_session(v, LV_PINYIN_IME_VOICE_ERR_NONE, 0);
            } else {
                IME_LOGW(TAG, "connection closed by peer before the final result");
                finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_CLOSED, 0);
            }
            goto done;
        }

        /* 2c. 取消优先级最高 */
        if (v->cancel_requested) {
            IME_LOGI(TAG, "cancelled by the user");
            finish_session(v, LV_PINYIN_IME_VOICE_ERR_NONE, 0);
            goto done;
        }

        /* 2d. 到点了（用户点结束 / 会话超 55 s）：发结束帧，转 FINISHING */
        const bool time_limit = (elapsed_ms >= IME_VOICE_MAX_SESSION_MS);
        if ((v->stop_requested || time_limit) && !sent_last) {
            if (time_limit) {
                IME_LOGW(TAG, "session reached the %u ms limit, stopping",
                         (unsigned)IME_VOICE_MAX_SESSION_MS);
                forced_stop = true;
            }
            const size_t n = ime_voice_build_last_frame(frame, frame_cap);
            if (n == 0 || host->ws_send_text(host->user, frame) != 0) {
                finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_SEND, 0);
                goto done;
            }
            sent_last = true;
            set_state(v, LV_PINYIN_IME_VOICE_FINISHING);
            finish_deadline_ms = t0 + IME_VOICE_FINISH_TIMEOUT_MS;
        }

        if (sent_last) {
            /* 等最终结果；超时就按已有文本收尾（有文本算成功，没文本算超时）。 */
            const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((int32_t)(now_ms - finish_deadline_ms) >= 0) {
                lock(v);
                const bool have_text = (v->text[0] != '\0');
                unlock(v);
                IME_LOGW(TAG, "no final result within %u ms (have_text=%d)",
                         (unsigned)IME_VOICE_FINISH_TIMEOUT_MS, (int)have_text);
                finish_session(v,
                               have_text ? LV_PINYIN_IME_VOICE_ERR_NONE
                                         : LV_PINYIN_IME_VOICE_ERR_TIMEOUT,
                               0);
                goto done;
            }
            (void)forced_stop;
            vTaskDelay(pdMS_TO_TICKS(IME_VOICE_TICK_MS));
            continue;
        }

        /* 2e. 读一帧麦克风数据并发出去。
         *
         * ★ 一次读不满就再读几次，直到凑够一帧或调用方明确返回 0/超时。
         *
         *   为什么：I2S / codec 驱动常按它自己的块大小（512、1024 B）返回，一次
         *   只给一小块。早期版本把这一小块当成"一整帧的 40 ms 音频"直接发出去，
         *   于是音频时间轴被拉长（发 1280 B 才等于 40 ms，实际只发了 512 B），
         *   服务端拿到的是**慢放且断续**的音频 —— 真机表现就是"明明说了话，
         *   却什么都没识别出来"，而且没有任何报错。
         *
         *   这里是兜底：即使调用方没在自己的回调里循环读满，组件也尽量把一帧凑齐。
         */
        size_t got = 0;
        bool audio_error = false;
        int last_rc = 0;
        const uint32_t read_deadline = t0 + IME_VOICE_TICK_MS * 2;
        while (got < frame_bytes) {
            last_rc = host->audio_read(host->user, pcm + got, frame_bytes - got, 5);
            if (last_rc < 0) {
                audio_error = true;
                break;
            }
            if (last_rc == 0) {
                /* 这一拍没数据了：就用已经拿到的部分（可能是 0），别死等。 */
                break;
            }
            got += (size_t)last_rc;

            /* 已经超过这一拍的时间预算，或者拿到了完整的一帧，就停 */
            const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((int32_t)(now_ms - read_deadline) >= 0) {
                break;
            }
        }

        if (audio_error) {
            v->consecutive_audio_errors++;
            /* 偶发一次（I2S 抖动、DMA 欠载）不该打断整段识别，连续失败才报错。 */
            if (v->consecutive_audio_errors >= 5) {
                IME_LOGE(TAG, "audio_read failed %d times in a row, aborting",
                         v->consecutive_audio_errors);
                finish_session(v, LV_PINYIN_IME_VOICE_ERR_AUDIO, 0);
                goto done;
            }
            IME_LOGW(TAG, "audio_read error (retry %d)", v->consecutive_audio_errors);
            got = 0;
        } else {
            v->consecutive_audio_errors = 0;
        }
        (void)last_rc;

        /* 音频电平：区分"真的在说话"和"读到的是静音/噪声"。
         *
         * 这一条是排查"说了话却没识别出来"的关键：服务端拿到一片静音时会老老实实
         * 回一个 status=2 的**空**结果，于是 text 为空、err=0，从日志上完全看不出
         * 问题在麦克风侧。这里把峰值换成 dBFS 打出来，一眼就能判断：
         *
         *   peak=3277  → 约 -20 dBFS   正常说话的下限
         *   peak=16384 → 约  -6 dBFS   大声说话
         *   peak=234   → 约 -43 dBFS   ← 实测见过这个值：比正常说话低 20~35 dB，
         *                                服务端 VAD 根本不会触发，所以识别为空
         *
         * 只打前 3 帧和每 2 秒一次的采样，不会刷屏。 */
        {
            const int16_t *s = (const int16_t *)pcm;
            const size_t nsamp = got / sizeof(int16_t);
            uint32_t peak = 0;
            uint64_t sum_sq = 0;
            for (size_t i = 0; i < nsamp; i++) {
                int32_t v32 = s[i];
                uint32_t a = (uint32_t)((v32 < 0) ? -v32 : v32);
                if (a > peak) {
                    peak = a;
                }
                sum_sq += (uint64_t)(a * a);
            }
            const uint32_t rms = (nsamp > 0) ? isqrt_u32(sum_sq / (uint64_t)nsamp) : 0u;

            if (peak > v->peak_max) {
                v->peak_max = peak;
            }

            if (frame_index < 3 || (frame_index % 50) == 0) {
                IME_LOGI(TAG, "audio frame %u: got=%u/%u B peak=%u (%d dBFS) rms=%u",
                         (unsigned)frame_index, (unsigned)got, (unsigned)frame_bytes,
                         (unsigned)peak, dbfs_from_amplitude(peak), (unsigned)rms);
            }

            /* 声学上持续低于约 -40 dBFS 基本就是静音（正常说话 -20 dBFS 起步）。 */
            if (dbfs_from_amplitude(peak) < -40) {
                v->silent_frames++;
            }
        }

        size_t n;
        if (elapsed_ms == 0) {
            n = ime_voice_build_first_frame(&v->params, pcm, got, frame, frame_cap);
        } else {
            n = ime_voice_build_audio_frame(&v->params, pcm, got, frame, frame_cap);
        }
        if (n == 0) {
            IME_LOGE(TAG, "frame build failed (%u bytes)", (unsigned)got);
            finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_SEND, 0);
            goto done;
        }
        if (host->ws_send_text(host->user, frame) != 0) {
            IME_LOGE(TAG, "ws_send_text failed");
            finish_session(v, LV_PINYIN_IME_VOICE_ERR_WS_SEND, 0);
            goto done;
        }
        frame_index++;

        elapsed_ms += IME_VOICE_TICK_MS;
        lock(v);
        v->audio_ms = elapsed_ms;
        unlock(v);

        /* 2f. 补足 40 ms 节拍（读 PCM 花掉的时间要从等待里扣掉） */
        const uint32_t t1 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const int32_t spent = (int32_t)(t1 - t0);
        int32_t wait = (int32_t)IME_VOICE_TICK_MS - spent;
        if (wait > 0) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)wait));
        } else {
            vTaskDelay(1);              /* 让出 CPU，避免把 LVGL 饿死 */
        }
    }

done:
    /*
     * 报一次这个任务真实的栈余量。
     *
     * 为什么值得写一行日志：上面那次 12.7 KB 的栈溢出，事后只能靠"崩了"来发现。
     * uxTaskGetStackHighWaterMark() 给的是**历史最小剩余量**（字），所以每次会话
     * 结束都会告诉我们"离溢出还有多远"：
     *
     *   - 余量稳定在几百字节以上  → 栈够
     *   - 余量接近 0 或报警        → 有新的大家伙混进栈里了，去查局部变量
     *
     * 只在会话结束时打一次，开销可以忽略。按字（4 字节）换算成字节显示。
     */
    const UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    /* ★ hwm 的单位是 StackType_t 个数，而这个端口里 StackType_t 就是 1 字节，
     *   所以它**已经**是字节数。写 sizeof(StackType_t) 只是把这件事说清楚，
     *   顺带在别的端口（StackType_t 是 4 字节的）上也算对。 */
    IME_LOGI(TAG, "voice task stack free at exit: %u B (high-water mark) -- "
                  "near zero means something large moved onto the stack",
             (unsigned)(hwm * sizeof(StackType_t)));

    v->task_running = false;
    v->task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------ 公开：配置 */

lv_pinyin_ime_voice_cfg_t lv_pinyin_ime_voice_cfg_default(void)
{
    lv_pinyin_ime_voice_cfg_t c;
    memset(&c, 0, sizeof(c));

    snprintf(c.appid, sizeof(c.appid), "%s", CONFIG_LV_PINYIN_IME_VOICE_APPID);
    snprintf(c.apikey, sizeof(c.apikey), "%s", CONFIG_LV_PINYIN_IME_VOICE_APIKEY);
    snprintf(c.apisecret, sizeof(c.apisecret), "%s", CONFIG_LV_PINYIN_IME_VOICE_APISECRET);
    snprintf(c.endpoint, sizeof(c.endpoint), "%s", CONFIG_LV_PINYIN_IME_VOICE_ENDPOINT);
    snprintf(c.language, sizeof(c.language), "%s", CONFIG_LV_PINYIN_IME_VOICE_LANGUAGE);
    snprintf(c.domain, sizeof(c.domain), "%s", CONFIG_LV_PINYIN_IME_VOICE_DOMAIN);
    snprintf(c.accent, sizeof(c.accent), "%s", CONFIG_LV_PINYIN_IME_VOICE_ACCENT);

    c.sample_rate = CONFIG_LV_PINYIN_IME_VOICE_SAMPLE_RATE;
    c.eos_ms = CONFIG_LV_PINYIN_IME_VOICE_EOS_MS;
    c.dynamic_correction = (CONFIG_LV_PINYIN_IME_VOICE_DWA != 0);
    c.punctuation = (CONFIG_LV_PINYIN_IME_VOICE_PUNCTUATION != 0);
    c.live_preview = (CONFIG_LV_PINYIN_IME_VOICE_LIVE_PREVIEW != 0);
    c.append_result = (CONFIG_LV_PINYIN_IME_VOICE_COMMIT_REPLACE == 0);
    c.host = NULL;
    return c;
}

bool lv_pinyin_ime_voice_supported(void)
{
    return (CONFIG_LV_PINYIN_IME_VOICE_ENABLE != 0);
}

lv_pinyin_ime_voice_tap_mode_t lv_pinyin_ime_voice_tap_mode(void)
{
    /* 用宏判断而不是 #if：三个取值都能编译，Kconfig 一改就生效。 */
#if defined(CONFIG_LV_PINYIN_IME_VOICE_TAP_PTT)
    return LV_PINYIN_IME_VOICE_TAP_PTT;
#elif defined(CONFIG_LV_PINYIN_IME_VOICE_TAP_BOTH)
    return LV_PINYIN_IME_VOICE_TAP_BOTH;
#else
    return LV_PINYIN_IME_VOICE_TAP_TOGGLE;
#endif
}

/* ------------------------------------------------------------ 生命周期 */

lv_pinyin_ime_voice_t *lv_pinyin_ime_voice_create(const lv_pinyin_ime_voice_cfg_t *cfg)
{
    /*
     * 这里不需要 #if CONFIG_..._VOICE_ENABLE 保护：整个 src/voice/ 只在开关打开时
     * 才编进组件（见 CMakeLists.txt）。而 lv_pinyin_ime_voice_supported() 仍然保留，
     * 因为界面侧要靠它决定"要不要画语音键"。
     */
    lv_pinyin_ime_voice_t *v = (lv_pinyin_ime_voice_t *)calloc(1, sizeof(*v));
    if (v == NULL) {
        return NULL;
    }
    v->lock = xSemaphoreCreateMutex();
    if (v->lock == NULL) {
        free(v);
        return NULL;
    }

    v->cfg = (cfg != NULL) ? *cfg : lv_pinyin_ime_voice_cfg_default();
    cfg_to_params(&v->cfg, &v->params);
    v->state = LV_PINYIN_IME_VOICE_IDLE;
    return v;
}

void lv_pinyin_ime_voice_destroy(lv_pinyin_ime_voice_t *v)
{
    if (v == NULL) {
        return;
    }
    /* 先停任务：它对 v 的字段有引用，必须等它退出再释放。 */
    if (v->task_running) {
        v->cancel_requested = true;
        v->stop_requested = true;
        for (int i = 0; i < 200 && v->task_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (v->task_running) {
            /* 任务卡在 audio_read 里（调用方的实现没能及时返回）。
             * 不强行删任务：vTaskDelete 掉一个正阻塞在自己的 I2S 读里的任务，
             * 会把调用方的驱动状态搞坏。这里只警告并要求调用方修 timeout。 */
            IME_LOGE(TAG, "voice task did not exit; make audio_read() honour timeout_ms");
        }
    }

    /* 清掉队列里剩下的帧 */
    size_t len = 0;
    char *msg = NULL;
    while ((msg = queue_pop(v, &len)) != NULL) {
        free(msg);
    }

    if (v->lock != NULL) {
        vSemaphoreDelete(v->lock);
    }
    free(v);
}

void lv_pinyin_ime_voice_set_config(lv_pinyin_ime_voice_t *v,
                                    const lv_pinyin_ime_voice_cfg_t *cfg)
{
    if (v == NULL || cfg == NULL) {
        return;
    }
    v->cfg = *cfg;
    cfg_to_params(&v->cfg, &v->params);
}

const lv_pinyin_ime_voice_cfg_t *lv_pinyin_ime_voice_get_config(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? &v->cfg : NULL;
}

void lv_pinyin_ime_voice_set_state_cb(lv_pinyin_ime_voice_t *v,
                                      lv_pinyin_ime_voice_cb_t cb, void *user)
{
    if (v == NULL) {
        return;
    }
    v->state_cb = cb;
    v->state_cb_user = user;
}

void lv_pinyin_ime_voice_set_result_cb(lv_pinyin_ime_voice_t *v,
                                       lv_pinyin_ime_voice_cb_t cb, void *user)
{
    if (v == NULL) {
        return;
    }
    v->result_cb = cb;
    v->result_cb_user = user;
}

/* ------------------------------------------------------------ 控制 */

int lv_pinyin_ime_voice_start(lv_pinyin_ime_voice_t *v)
{
    if (v == NULL) {
        return -LV_PINYIN_IME_VOICE_ERR_NO_HOST;
    }
    if (v->task_running) {
        IME_LOGW(TAG, "already listening");
        return -LV_PINYIN_IME_VOICE_ERR_BUSY;
    }
    if (v->cfg.host == NULL || v->cfg.host->ws_open == NULL ||
        v->cfg.host->ws_send_text == NULL || v->cfg.host->audio_read == NULL) {
        IME_LOGE(TAG, "host callbacks are incomplete");
        lock(v);
        v->err = LV_PINYIN_IME_VOICE_ERR_NO_HOST;
        unlock(v);
        set_state(v, LV_PINYIN_IME_VOICE_ERROR);
        return -LV_PINYIN_IME_VOICE_ERR_NO_HOST;
    }
    if (!ime_voice_params_valid(&v->params)) {
        IME_LOGE(TAG, "AppID / APIKey / APISecret not configured "
                      "(menuconfig -> LVGL Pinyin IME -> Voice input)");
        lock(v);
        v->err = LV_PINYIN_IME_VOICE_ERR_NOT_CONFIGURED;
        unlock(v);
        set_state(v, LV_PINYIN_IME_VOICE_ERROR);
        return -LV_PINYIN_IME_VOICE_ERR_NOT_CONFIGURED;
    }

    /* 复位本次会话的状态 */
    lock(v);
    ime_voice_transcript_reset(&v->transcript);
    v->text[0] = '\0';
    v->result_ready = false;
    v->err = LV_PINYIN_IME_VOICE_ERR_NONE;
    v->server_code = 0;
    v->audio_ms = 0;
    v->msg_head = 0;
    v->msg_tail = 0;
    v->msg_dropped = 0;
    v->stop_requested = false;
    v->cancel_requested = false;
    v->ws_open_done = false;
    v->ws_closed_by_peer = false;
    v->consecutive_audio_errors = 0;
    v->silent_frames = 0;
    v->peak_max = 0;
    unlock(v);

    set_state(v, LV_PINYIN_IME_VOICE_CONNECTING);

    v->task_running = true;
    /*
     * 栈：**6144 字节**（不是拍脑袋，是用 -fstack-usage 量出来的）。
     *
     * ⚠ 这里踩过两次，记清楚，免得再改回去：
     *
     *   第一次（写成 4096）崩在**签名路径**：build_auth_url() 的局部数组
     *   （authorization[512] + enc_auth[768] + auth_origin[320] + enc_host[192]
     *     + origin[320] + host_str[128] ≈ 2.2 KB）叠上日志的 snprintf 就超了。
     *
     *   第二次（已经调到 8192）崩在**返回帧解析**：
     *     Guru Meditation Error: Core 0 panic'ed (Stack protection fault)
     *     Detected in task "ime_voice" at handle_reply (ime_voice.c:344)
     *     Stack bounds: 0x2f04310c - 0x2f045100   ← 8192 字节
     *     Stack pointer: 0x2f041e50               ← 已越过下边界约 4.8 KB
     *   根因不是"栈不够大"，而是 handle_reply() 里那句
     *   `ime_voice_reply_t r;` —— **这个结构体单独就 12 744 字节**
     *   （segs[32] × 392 B）。栈开到多大都不该放它。
     *
     *   所以真正的修法是把大缓冲搬进会话的工作区（ime_voice_scratch_t，堆上）：
     *   reply 12.7 KB、拼帧缓冲 3.2 KB、PCM 1.25 KB、整句快照 640 B，合计约 17.9 KB，
 *   随会话 malloc 一次、任务循环反复复用。
     *
     * 量出来的实际栈深（tools/check_stack_usage.ps1，交叉编译器 -fstack-usage）：
     *
     *   voice_task                    112 B
     *   签名路径  build_auth_url     2464 B   ← 最坏的一条
     *             └ hmac_sha256       368 B
     *             └ sha256_block      352 B
     *             合计 ≈ 3248 B
     *   收帧路径  handle_reply        112 B → parse_reply 80 → parse_result 144
     *             → parse_seg 64 → parse_cw 96                  合计 ≈ 528 B
     *   拼帧路径  build_first_frame    96 B                       合计 ≈ 208 B
     *
     * 6144 B 相对最坏值 3248 B 留了约 2.9 KB（≈90%）余量，用来吸收
     * ESP_LOG 的 snprintf、mbedtls/psa 初始化等库调用自己的栈 —— 那些不在我们
     * 的 .su 统计里。任务只在一次识别会话期间存在（结束即 vTaskDelete），
     * 多留 1 KB 比再崩一次划算。
     *
     * 优先级仍低于 LVGL 主任务（app_main = 1），避免 40 ms 的节拍把渲染挤掉。
     */
    const BaseType_t ok = xTaskCreate(voice_task, "ime_voice", IME_VOICE_TASK_STACK, v, 2,
                                      &v->task);
    if (ok != pdPASS) {
        v->task_running = false;
        lock(v);
        v->err = LV_PINYIN_IME_VOICE_ERR_NO_HOST;
        unlock(v);
        set_state(v, LV_PINYIN_IME_VOICE_ERROR);
        return -LV_PINYIN_IME_VOICE_ERR_NO_HOST;
    }
    return 0;
}

void lv_pinyin_ime_voice_stop(lv_pinyin_ime_voice_t *v)
{
    if (v == NULL || !v->task_running) {
        return;
    }
    v->stop_requested = true;
}

void lv_pinyin_ime_voice_cancel(lv_pinyin_ime_voice_t *v)
{
    if (v == NULL || !v->task_running) {
        return;
    }
    v->cancel_requested = true;
    v->stop_requested = true;
}

/* ------------------------------------------------------------ 查询 */

lv_pinyin_ime_voice_state_t lv_pinyin_ime_voice_state(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? v->state : LV_PINYIN_IME_VOICE_IDLE;
}

lv_pinyin_ime_voice_err_t lv_pinyin_ime_voice_error(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? v->err : LV_PINYIN_IME_VOICE_ERR_NONE;
}

int lv_pinyin_ime_voice_server_code(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? v->server_code : 0;
}

const char *lv_pinyin_ime_voice_state_text(const lv_pinyin_ime_voice_t *v)
{
    if (v == NULL) {
        return "";
    }
    switch (v->state) {
    case LV_PINYIN_IME_VOICE_CONNECTING:
        return "正在连接";
    case LV_PINYIN_IME_VOICE_LISTENING:
        return "聆听中";
    case LV_PINYIN_IME_VOICE_FINISHING:
        return "识别中";
    case LV_PINYIN_IME_VOICE_DONE:
        return "完成";
    case LV_PINYIN_IME_VOICE_ERROR: {
        const char *t = err_text(v->err);
        if (t == NULL) {
            return "语音出错";
        }
        /* 服务端错误：把错误码一起显示，便于对文档查表。 */
        if (v->err == LV_PINYIN_IME_VOICE_ERR_SERVER && v->server_code != 0) {
            static char buf[48];
            snprintf(buf, sizeof(buf), "语音错误 %d", v->server_code);
            return buf;
        }
        return t;
    }
    case LV_PINYIN_IME_VOICE_IDLE:
    default:
        return "";
    }
}

const char *lv_pinyin_ime_voice_text(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? v->text : "";
}

bool lv_pinyin_ime_voice_result_ready(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) && v->result_ready;
}

uint32_t lv_pinyin_ime_voice_audio_ms(const lv_pinyin_ime_voice_t *v)
{
    return (v != NULL) ? v->audio_ms : 0;
}

/* ------------------------------------------------------------ 网络回调 */

void lv_pinyin_ime_voice_ws_message(lv_pinyin_ime_voice_t *v, const char *json, size_t len)
{
    if (v == NULL || json == NULL || len == 0) {
        return;
    }
    queue_push(v, json, len);
}

void lv_pinyin_ime_voice_ws_closed(lv_pinyin_ime_voice_t *v, int err)
{
    if (v == NULL) {
        return;
    }
    if (err != 0) {
        IME_LOGW(TAG, "websocket closed with error %d", err);
    }
    v->ws_closed_by_peer = true;
}
