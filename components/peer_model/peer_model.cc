#include "peer_model.h"

#include "peer_model_window.h"
#include "peer_judge.h"

#include "es7210_mic.h"
#include "esp_dsp_fft.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fusion_model.h"
#include "fusion_native.h"
#include "hamic_frontend.h"

#include <cmath>
#include <cstring>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "peer_model";

/** 模型任务忙大约 1 秒时，50 ms 一帧的快照还能堆在这里。 */
#define SNAP_Q         48
#define RING_SAMPLES   (16 * 16000)
#define AUDIO_SAMPLES  160000
#define MIC_FRAMES     256
#define ARENA_BYTES    (4 * 1024 * 1024)

static QueueHandle_t s_snap_q;
static TaskHandle_t s_model_task;
static volatile bool s_overflow;

static int16_t *s_ring;
static size_t s_wr;
static size_t s_filled;
static SemaphoreHandle_t s_ring_mu;

static int16_t *s_pcm;
static float *s_audio;
static HamicWorkspace *s_front;
static FusionSensorWorkspace *s_sensor_ws;
static uint8_t *s_arena;
static tflite::MicroMutableOpResolver<6> s_resolver;
static tflite::MicroInterpreter *s_interpreter;
static TfLiteTensor *s_in;
static TfLiteTensor *s_out;

/** 从环里拷最近 10 秒左声道。锁只包住拷贝，推理在锁外。不满 10 秒返回 false。 */
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

/** 交错立体声只留 MIC1。一次写入大约 256 点，持锁时间很短。 */
static void push_left(const int16_t *stereo, int frames)
{
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    for (int i = 0; i < frames; i++) {
        s_ring[s_wr] = stereo[i * 2];
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

static void softmax6(const float *logits, float *prob)
{
    float max = logits[0];
    for (int i = 1; i < 6; i++) {
        if (logits[i] > max) {
            max = logits[i];
        }
    }
    float sum = 0.0f;
    for (int i = 0; i < 6; i++) {
        prob[i] = std::exp(logits[i] - max);
        sum += prob[i];
    }
    for (int i = 0; i < 6; i++) {
        prob[i] /= sum;
    }
}

/** 声音编码失败或张量对不上时，这一拍不出结果。上一拍类别留给窗口自己保留。 */
static bool engine_ready(void)
{
    if (s_interpreter != NULL) {
        return true;
    }
    s_arena = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_audio = static_cast<float *>(heap_caps_aligned_alloc(16, HAMIC_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_pcm = static_cast<int16_t *>(heap_caps_aligned_alloc(16, AUDIO_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_front = static_cast<HamicWorkspace *>(heap_caps_aligned_alloc(16, sizeof(HamicWorkspace), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_sensor_ws = static_cast<FusionSensorWorkspace *>(
        heap_caps_calloc(1, sizeof(FusionSensorWorkspace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_arena == NULL || s_audio == NULL || s_pcm == NULL || s_front == NULL || s_sensor_ws == NULL) {
        ESP_LOGE(TAG, "推理缓冲申请失败");
        return false;
    }
    if (hamic_esp_dsp_fft_init() != 0) {
        ESP_LOGE(TAG, "FFT 初始化失败");
        return false;
    }
    if (s_resolver.AddPad() != kTfLiteOk || s_resolver.AddConv2D() != kTfLiteOk ||
        s_resolver.AddDepthwiseConv2D() != kTfLiteOk || s_resolver.AddConcatenation() != kTfLiteOk ||
        s_resolver.AddAveragePool2D() != kTfLiteOk || s_resolver.AddReshape() != kTfLiteOk) {
        ESP_LOGE(TAG, "算子注册失败");
        return false;
    }
    const tflite::Model *model = tflite::GetModel(fusion_model::audio_encoder_tflite);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "模型版本不匹配");
        return false;
    }
    s_interpreter = new tflite::MicroInterpreter(model, s_resolver, s_arena, ARENA_BYTES);
    if (s_interpreter == NULL || s_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors 失败");
        return false;
    }
    s_in = s_interpreter->input(0);
    s_out = s_interpreter->output(0);
    if (s_in->type != kTfLiteFloat32 || s_out->type != kTfLiteFloat32 ||
        s_in->bytes != HAMIC_FEATURES * sizeof(float) || s_out->bytes != 96 * sizeof(float)) {
        ESP_LOGE(TAG, "输入输出张量不符合 10 秒编码器");
        return false;
    }
    return true;
}

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
    hamic_pcm16_to_float(s_pcm, s_audio, AUDIO_SAMPLES);
    if (hamic_frontend_16k(s_audio, AUDIO_SAMPLES, s_in->data.f, s_front, hamic_esp_dsp_fft512) != HAMIC_OK) {
        ESP_LOGE(TAG, "声音前端失败，这一拍跳过");
        return;
    }
    if (s_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "声音编码器失败，这一拍跳过");
        return;
    }
    for (int i = 0; i < n; i++) {
        FusionSensorInput in{};
        memcpy(in.radar, zones[i].radar, sizeof(in.radar));
        memcpy(in.env, zones[i].env, sizeof(in.env));
        in.epoch_sec = zones[i].epoch_sec;
        in.zone = zones[i].model_zone;
        in.room = zones[i].room;
        float sensor[32];
        float logits[6];
        float prob[6];
        if (!fusion_sensor_encode(&in, sensor, s_sensor_ws)) {
            continue;
        }
        fusion_classify(s_out->data.f, sensor, logits);
        softmax6(logits, prob);
        bool finite = true;
        for (int c = 0; c < 6; c++) {
            if (!std::isfinite(prob[c])) {
                finite = false;
            }
        }
        if (!finite) {
            continue;
        }
        int top = peer_model_top_class(prob);
        peer_event_t ev;
        if (!peer_model_window_event(zones[i].zone_id, top, &ev)) {
            continue;
        }
        /* 入队失败不记上一拍，下一拍第一名还是这个类会再试。 */
        if (peer_judge_push(&ev)) {
            peer_model_window_event_sent(zones[i].zone_id, top);
        }
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
    const es7210_mic_pins_t pins = {
        .sda = GPIO_NUM_1,
        .scl = GPIO_NUM_2,
        .mclk = GPIO_NUM_6,
        .bclk = GPIO_NUM_4,
        .lrck = GPIO_NUM_5,
        .sdo = GPIO_NUM_3,
    };
    while (es7210_mic_init(&pins) != ESP_OK) {
        ESP_LOGW(TAG, "未找到 ES7210，2 秒后重试");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    int16_t buf[MIC_FRAMES * 2];
    for (;;) {
        if (es7210_mic_read(buf, sizeof(buf)) != ESP_OK) {
            continue;
        }
        push_left(buf, MIC_FRAMES);
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
