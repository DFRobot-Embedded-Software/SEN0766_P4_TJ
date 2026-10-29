#include "peer_slave.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/** 上层回调。只在 peer_uart 任务里调用。 */
static peer_slave_sink_t s_sink;
/**
 * 保护 uart_write_bytes。当前只有本任务发 0x20，互斥是为了以后别的任务也能发，
 * 避免两帧字节交错。0x10/0x11 是主机下行，从机不发。
 */
static SemaphoreHandle_t s_tx_mu;

/**
 * 把队列里的事件逐条组 0x20 发出去。
 * 长度不是 sizeof(peer_event_t) 就停：无事件返回 0，残帧也不上线。
 * seq 用刚收到的 0x10 序号，方便主机把事件对上那一拍快照。
 * 写完才 reply_done，写失败前不会把事件弹出。
 */
static void tx_event(uint8_t seq)
{
    static uint8_t body[sizeof(peer_event_t)];
    static uint8_t frame[PEER_FRAME_MAX];
    if (s_sink.reply_payload == NULL) {
        return;
    }
    for (;;) {
        uint16_t n = s_sink.reply_payload(seq, body, sizeof(body), s_sink.user);
        if (n != sizeof(peer_event_t)) {
            break;
        }
        size_t flen = peer_frame_encode(PEER_MSG_EVENT, seq, body, n, frame, sizeof(frame));
        if (flen == 0) {
            break;
        }
        xSemaphoreTake(s_tx_mu, portMAX_DELAY);
        uart_write_bytes(PEER_UART_PORT, frame, flen);
        xSemaphoreGive(s_tx_mu);
        if (s_sink.reply_done) {
            s_sink.reply_done(s_sink.user);
        }
    }
}

/**
 * 一帧 CRC 已通过。
 * 只认 0x10 / 0x11，其它类型丢掉。
 * 0x10 刷新“最近一次快照”时间，并清掉空闲标记；处理完立刻把积压的 0x20 发出去。
 * 0x11 只交给上层暂存，不发应答。
 */
static void handle_frame(uint8_t msg, uint8_t seq, const uint8_t *payload, uint16_t plen,
                         TickType_t *last_snap, bool *idle)
{
    if (msg != PEER_MSG_ENV_CFG && msg != PEER_MSG_SNAPSHOT) {
        return;
    }
    if (msg == PEER_MSG_SNAPSHOT) {
        *last_snap = xTaskGetTickCount();
        *idle = false;
    }
    if (s_sink.on_rx) {
        s_sink.on_rx(msg, payload, plen, seq, s_sink.user);
    }
    if (msg == PEER_MSG_SNAPSHOT) {
        tx_event(seq);
    }
}

/**
 * 收包任务。
 * uart_read 超时 100 ms，用来兼做空闲检测，不另开定时器。
 * 字节先堆进 acc，再按 peer_frame_next 拆。
 * 不够一帧就留着；CRC 错则丢掉 next 给出的前缀再扫。
 * acc 装不下时整段丢掉，避免半帧永远卡在缓冲头上。
 * 超过 PEER_UART_IDLE_MS 没有 0x10，只通知一次 on_idle。
 */
static void slave_task(void *arg)
{
    (void)arg;
    uint8_t acc[PEER_FRAME_MAX * 2];
    uint8_t chunk[256];
    size_t acc_n = 0;
    TickType_t last_snap = xTaskGetTickCount();
    bool idle = false;

    while (1) {
        int n = uart_read_bytes(PEER_UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        if (n > 0) {
            if (acc_n + (size_t)n > sizeof(acc)) {
                acc_n = 0;
            }
            memcpy(acc + acc_n, chunk, (size_t)n);
            acc_n += (size_t)n;
            while (acc_n >= 9) {
                size_t drop = 0;
                uint8_t msg = 0, seq = 0;
                const uint8_t *payload = NULL;
                uint16_t plen = 0;
                bool ok = peer_frame_next(acc, acc_n, &drop, &msg, &seq, &payload, &plen);
                if (ok) {
                    size_t total = 9u + (size_t)plen;
                    handle_frame(msg, seq, payload, plen, &last_snap, &idle);
                    size_t used = drop + total;
                    memmove(acc, acc + used, acc_n - used);
                    acc_n -= used;
                    continue;
                }
                if (drop == 0) {
                    break;
                }
                memmove(acc, acc + drop, acc_n - drop);
                acc_n -= drop;
            }
        }

        if (!idle && (xTaskGetTickCount() - last_snap) >= pdMS_TO_TICKS(PEER_UART_IDLE_MS)) {
            idle = true;
            if (s_sink.on_idle) {
                s_sink.on_idle(s_sink.user);
            }
        }
    }
}

/** 安装 UART 驱动并启动收包任务。RX 环形缓冲见 PEER_UART_RX_BUF，TX 缓冲按一帧上限留。 */
esp_err_t peer_slave_start(const peer_slave_sink_t *sink)
{
    if (sink == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_sink = *sink;
    s_tx_mu = xSemaphoreCreateMutex();
    if (s_tx_mu == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t cfg = {
        .baud_rate = PEER_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(PEER_UART_PORT, PEER_UART_RX_BUF, PEER_FRAME_MAX, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(PEER_UART_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(PEER_UART_PORT, PEER_UART_TX, PEER_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(slave_task, "peer_uart", 8192, NULL, 10, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
