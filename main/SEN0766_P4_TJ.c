#include "peer_event.h"
#include "peer_slave.h"

#include <stdint.h>

#include "es7210_mic.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/** 本板 ES7210 接线：SDA1 SCL2 SDO3 BCLK4 LRCK5 MCLK6。 */
static const es7210_mic_pins_t s_pins = {
    .sda = GPIO_NUM_1,
    .scl = GPIO_NUM_2,
    .sdo = GPIO_NUM_3,
    .bclk = GPIO_NUM_4,
    .lrck = GPIO_NUM_5,
    .mclk = GPIO_NUM_6,
};

/** 一次采集的立体声采样点数。16 kHz 下约 16 ms。 */
#define FRAME_SAMPLES    256
#define MIC_QUEUE_LEN    10
#define MIC_TASK_STACK   4096
#define MIC_TASK_PRIO    5

typedef struct {
    int16_t pcm[FRAME_SAMPLES * ES7210_MIC_CHANNELS];
} mic_frame_t;

static const char *TAG = "app";
static QueueHandle_t s_queue;
static mic_frame_t s_cap;
static mic_frame_t s_drop;

/** 连续读 I2S。每 2 秒把当前帧入队，读的过程不休眠。 */
static void mic_capture_task(void *arg)
{
    (void)arg;
    /* 未接 ES7210 时 init 会失败。这里不能用 ESP_ERROR_CHECK，否则会 abort 复位。 */
    while (es7210_mic_init(&s_pins) != ESP_OK) {
        ESP_LOGW(TAG, "未找到 ES7210，2 秒后重试");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    TickType_t last_enqueue = xTaskGetTickCount();
    while (1) {
        ESP_ERROR_CHECK(es7210_mic_read(s_cap.pcm, sizeof(s_cap.pcm)));
        if ((xTaskGetTickCount() - last_enqueue) < pdMS_TO_TICKS(2000)) {
            continue;
        }
        last_enqueue = xTaskGetTickCount();
        /* 队列已满时丢掉最旧的一帧，保留最近 10 次入队结果。 */
        if (uxQueueSpacesAvailable(s_queue) == 0) {
            xQueueReceive(s_queue, &s_drop, 0);
        }
        ESP_ERROR_CHECK(xQueueSend(s_queue, &s_cap, 0) == pdTRUE ? ESP_OK : ESP_FAIL);
    }
}

/** UART 入帧。先交给事件组件；以后其它处理在后面再拷一份 payload 即可。 */
static void on_peer_rx(uint8_t msg_type, const uint8_t *payload, uint16_t len, uint8_t seq, void *user)
{
    peer_event_on_rx(msg_type, payload, len, seq, user);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_level_set("peer_event", ESP_LOG_INFO);
    if (peer_event_selfcheck() == ESP_OK) {
        static const peer_slave_sink_t sink = {
            .on_rx = on_peer_rx,
            .on_idle = peer_event_on_idle,
            .reply_payload = peer_event_reply_payload,
            .reply_done = peer_event_reply_done,
        };
        (void)peer_slave_start(&sink);
    }
    s_queue = xQueueCreate(MIC_QUEUE_LEN, sizeof(mic_frame_t));
    ESP_ERROR_CHECK(s_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(mic_capture_task, "mic_cap", MIC_TASK_STACK, NULL, MIC_TASK_PRIO, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_FAIL);
}
