#include "peer_model.h"

#include "peer_model_window.h"
#include "peer_judge.h"

#include "es7243e_mic.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fusion_model.h"
#include "hamic_frontend.h"

#include <cstring>

#ifndef PEER_MODEL_AUDIO_DUMP
#define PEER_MODEL_AUDIO_DUMP 1
#endif

#if PEER_MODEL_AUDIO_DUMP
#include "driver/uart.h"
#endif

static const char *TAG = "peer_model";

/** 模型任务忙大约 1 秒时，50 ms 一帧的快照还能堆在这里。 */
#define SNAP_Q         48
#define RING_SAMPLES   (16 * 16000)
#define AUDIO_SAMPLES  160000
#define MIC_FRAMES     256

static QueueHandle_t s_snap_q;
static TaskHandle_t s_model_task;
static volatile bool s_overflow;

static int16_t *s_ring;
static size_t s_wr;
static size_t s_filled;
static SemaphoreHandle_t s_ring_mu;

static int16_t *s_pcm;
static float *s_audio;
static float *s_mel;
static HamicFrontendWorkspace *s_front;
static uint8_t *s_workspace;
static size_t s_workspace_bytes;

/** 从环里拷最近 10 秒单声道 PCM。锁只包住拷贝，推理在锁外。不满 10 秒返回 false。 */
static bool copy_latest_audio(int16_t *dst)
{
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    bool ok = s_filled >= AUDIO_SAMPLES;
    if (ok) {
        size_t start = (s_wr + RING_SAMPLES - AUDIO_SAMPLES) % RING_SAMPLES;
        size_t first = RING_SAMPLES - start;
        if (first >= AUDIO_SAMPLES) {
            memcpy(dst, s_ring + start, AUDIO_SAMPLES * sizeof(int16_t));
        } else {
            memcpy(dst, s_ring + start, first * sizeof(int16_t));
            memcpy(dst + first, s_ring, (AUDIO_SAMPLES - first) * sizeof(int16_t));
        }
    }
    xSemaphoreGive(s_ring_mu);
    return ok;
}

/** 单声道 PCM16 写入环形缓冲。一次大约 256 点，持锁时间很短。 */
static void push_pcm(const int16_t *pcm, int frames)
{
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    for (int i = 0; i < frames; i++) {
        s_ring[s_wr] = pcm[i];
        s_wr++;
        if (s_wr >= RING_SAMPLES) {
            s_wr = 0;
        }
        if (s_filled < RING_SAMPLES) {
            s_filled++;
        }
    }
    xSemaphoreGive(s_ring_mu);
}

/** 声音编码失败时，这一拍不出结果。上一拍类别留给窗口自己保留。 */
static bool engine_ready(void)
{
    if (s_workspace != NULL) {
        return true;
    }
    s_workspace_bytes = hamic_workspace_bytes();
    s_workspace = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(16, s_workspace_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_audio = static_cast<float *>(
        heap_caps_aligned_alloc(16, HAMIC_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_mel = static_cast<float *>(
        heap_caps_aligned_alloc(16, HAMIC_FEATURES * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_pcm = static_cast<int16_t *>(
        heap_caps_aligned_alloc(16, AUDIO_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_front = static_cast<HamicFrontendWorkspace *>(
        heap_caps_aligned_alloc(16, sizeof(HamicFrontendWorkspace), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_workspace == NULL || s_audio == NULL || s_mel == NULL || s_pcm == NULL || s_front == NULL) {
        ESP_LOGE(TAG, "推理缓冲申请失败");
        return false;
    }
    return true;
}

/** 这一拍 3 个类的概率。概率已是 softmax。类名没变、未入队时也打。 */
static void log_model_output(int32_t zone_id, const float prob[HAMIC_CLASS_COUNT], int top, int queued)
{
    ESP_LOGI(TAG, "model out zone_id=%ld top=%s queued=%d", (long)zone_id, hamic_class_names[top], queued);
    ESP_LOGI(TAG, "  %s=%.3f %s=%.3f %s=%.3f", hamic_class_names[0], prob[0], hamic_class_names[1], prob[1],
             hamic_class_names[2], prob[2]);
}

/** 这一拍送给融合模型的传感器表。每区 10 行，旧秒在前。 */
static void log_model_input(const peer_model_zone_in_t *z)
{
    char purpose[PEER_STR16 + 1];
    memcpy(purpose, z->purpose, PEER_STR16);
    purpose[PEER_STR16] = '\0';
    ESP_LOGI(TAG, "model in zone_id=%ld purpose=\"%s\" model_zone=%d room=%d epoch=%.0f",
             (long)z->zone_id, purpose, z->model_zone, z->room, z->epoch_sec);
    for (int t = 0; t < 10; t++) {
        const float *radar = z->radar + t * 5;
        const float *env = z->env + t * 5;
        ESP_LOGI(TAG,
                 "  t=%d radar still=%.0f micro=%.0f slow=%.0f fast=%.0f event=%.0f | env temp=%.1f rh=%.1f co2=%.0f lux=%.0f db=%.1f",
                 t, radar[0], radar[1], radar[2], radar[3], radar[4], env[0], env[1], env[2], env[3], env[4]);
    }
}

static void fill_sensor(const peer_model_zone_in_t *z, HamicSensorInput *in)
{
    memset(in, 0, sizeof(*in));
    for (int t = 0; t < 10; t++) {
        for (int c = 0; c < 5; c++) {
            in->radar[t][c] = z->radar[t * 5 + c];
            in->env[t][c] = z->env[t * 5 + c];
        }
    }
    in->epoch_seconds = z->epoch_sec;
    in->zone = z->model_zone;
    in->room = z->room;
}

#if PEER_MODEL_AUDIO_DUMP
static bool s_dump_on;

static void dump_init(void)
{
    uart_config_t cfg = {};
    cfg.baud_rate = 921600;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(UART_NUM_2, 256, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(UART_NUM_2, &cfg) != ESP_OK ||
        uart_set_pin(UART_NUM_2, GPIO_NUM_24, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) !=
            ESP_OK) {
        ESP_LOGE(TAG, "音频串口初始化失败");
        return;
    }
    s_dump_on = true;
}

/** 一块 4096 字节约 45 ms。驱动无写超时，整段用 tick 卡 5 秒。 */
static bool dump_write(const void *src, size_t len, TickType_t t0)
{
    const uint8_t *p = static_cast<const uint8_t *>(src);
    size_t off = 0;
    while (off < len) {
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(5000)) {
            return false;
        }
        size_t n = len - off;
        if (n > 4096) {
            n = 4096;
        }
        int w = uart_write_bytes(UART_NUM_2, p + off, n);
        if (w <= 0) {
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
}

static void dump_pcm(const int16_t *pcm)
{
    if (!s_dump_on) {
        return;
    }
    uint8_t hdr[12];
    uint32_t rate = 16000;
    uint32_t count = AUDIO_SAMPLES;
    memcpy(hdr, "PCM1", 4);
    memcpy(hdr + 4, &rate, 4);
    memcpy(hdr + 8, &count, 4);
    TickType_t t0 = xTaskGetTickCount();
    if (!dump_write(hdr, sizeof(hdr), t0) || !dump_write(pcm, AUDIO_SAMPLES * sizeof(int16_t), t0)) {
        ESP_LOGW(TAG, "音频导出失败");
    }
}
#endif

/**
 * 这一拍先取走满窗的区，再拷声音。
 * 声环不满 10 秒、前端失败或编码器失败：不调用 event，上一拍类别不变。
 * 某个区编码失败只跳过该区。前面已经 push 成功的区保持已上报。
 */
static void run_beat(void)
{
    peer_model_zone_in_t zones[16];
    int n = peer_model_window_zones(zones, 16);
    if (n <= 0) {
        return;
    }
    if (!copy_latest_audio(s_pcm)) {
        return;
    }
#if PEER_MODEL_AUDIO_DUMP
    dump_pcm(s_pcm);
#endif
    hamic_pcm16_to_float(s_pcm, s_audio, AUDIO_SAMPLES);
    if (hamic_frontend_16k(s_audio, AUDIO_SAMPLES, s_mel, s_front, hamic_fft512_portable) != HAMIC_OK) {
        ESP_LOGE(TAG, "声音前端失败，这一拍跳过");
        return;
    }
    float audio96[96];
    if (hamic_encode_audio(s_mel, s_workspace, s_workspace_bytes, audio96) != HAMIC_OK) {
        ESP_LOGE(TAG, "声音编码器失败，这一拍跳过");
        return;
    }
    ESP_LOGI(TAG, "audio in samples=%d rate=16000 mels=%d first4=%.3f %.3f %.3f %.3f", AUDIO_SAMPLES, HAMIC_FEATURES,
             s_mel[0], s_mel[1], s_mel[2], s_mel[3]);
    for (int i = 0; i < n; i++) {
        log_model_input(&zones[i]);
        HamicSensorInput in;
        fill_sensor(&zones[i], &in);
        float sensor32[32];
        HamicOutput output;
        if (hamic_encode_sensor(&in, s_workspace, s_workspace_bytes, sensor32) != HAMIC_OK) {
            ESP_LOGI(TAG, "model out zone_id=%ld encode rejected", (long)zones[i].zone_id);
            continue;
        }
        if (hamic_fuse_features(audio96, sensor32, &output) != HAMIC_OK) {
            ESP_LOGI(TAG, "model out zone_id=%ld fusion rejected", (long)zones[i].zone_id);
            continue;
        }
        int top = peer_model_top_class(output.probabilities);
        int queued = 0;
        peer_event_t ev;
        /* 第一名不超过 0.6 不入队，上一拍类别保持不变。 */
        if (peer_model_report_ok(output.probabilities, top) &&
            peer_model_window_event(zones[i].zone_id, top, &ev)) {
            /* 入队失败不记上一拍，下一拍第一名还是这个类会再试。 */
            if (peer_judge_push(&ev)) {
                peer_model_window_event_sent(zones[i].zone_id, top);
                queued = 1;
            }
        }
        log_model_output(zones[i].zone_id, output.probabilities, top, queued);
    }
}

static void discard_queued(void)
{
    peer_snapshot_t dump;
    while (xQueueReceive(s_snap_q, &dump, 0) == pdTRUE) {
    }
}

static void model_task(void *arg)
{
    (void)arg;
    if (!engine_ready()) {
        ESP_LOGE(TAG, "模型没起来，不再推理");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    for (;;) {
        /* 队列曾经满过：里面的快照缺了一帧，整段丢掉并清空窗口。 */
        if (s_overflow) {
            s_overflow = false;
            ulTaskNotifyTake(pdTRUE, 0);
            discard_queued();
            peer_model_window_invalidate();
        }
        peer_snapshot_t snap;
        if (xQueueReceive(s_snap_q, &snap, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (s_overflow) {
            continue;
        }
        /* 返回 true 就立刻推理，不要先把队列里后面的秒抽干。 */
        if (peer_model_window_on_snapshot(&snap)) {
            run_beat();
        }
    }
}

/** 持续读 I2S，避免推理时 DMA 溢出。init 失败只重试，不复位。 */
static void mic_task(void *arg)
{
    (void)arg;
    const es7243e_mic_pins_t pins = {
        .sda = GPIO_NUM_1,
        .scl = GPIO_NUM_2,
        .mclk = GPIO_NUM_6,
        .bclk = GPIO_NUM_5,
        .lrck = GPIO_NUM_4,
        .sdo = GPIO_NUM_3,
    };
    while (es7243e_mic_init(&pins) != ESP_OK) {
        ESP_LOGW(TAG, "未找到 ES7243E，2 秒后重试");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    int16_t buf[MIC_FRAMES];
    for (;;) {
        if (es7243e_mic_read(buf, sizeof(buf)) != ESP_OK) {
            continue;
        }
        push_pcm(buf, MIC_FRAMES);
    }
}

extern "C" void peer_model_start(void)
{
    if (s_snap_q != NULL) {
        return;
    }
    s_snap_q = xQueueCreate(SNAP_Q, sizeof(peer_snapshot_t));
    s_ring_mu = xSemaphoreCreateMutex();
    s_ring = static_cast<int16_t *>(heap_caps_calloc(RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_snap_q == NULL || s_ring_mu == NULL || s_ring == NULL) {
        ESP_LOGE(TAG, "队列或声环申请失败");
        return;
    }
#if PEER_MODEL_AUDIO_DUMP
    dump_init();
#endif
    if (xTaskCreate(mic_task, "peer_mic", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(model_task, "peer_model", 16384, NULL, 3, &s_model_task) != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
    }
}

extern "C" void peer_model_submit(const peer_snapshot_t *snap)
{
    if (snap == NULL || s_snap_q == NULL) {
        return;
    }
    if (xQueueSend(s_snap_q, snap, 0) == pdTRUE) {
        return;
    }
    /* 新来的这一帧不进队列。模型任务醒来后丢掉积压并清空窗口。 */
    s_overflow = true;
    if (s_model_task != NULL) {
        xTaskNotifyGive(s_model_task);
    }
}
