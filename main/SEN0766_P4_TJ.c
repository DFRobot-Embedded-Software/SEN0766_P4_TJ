#include "peer_event.h"
#include "peer_model.h"
#include "peer_slave.h"

#include <string.h>

#include "esp_log.h"

/**
 * UART 入帧。先交给事件组件。
 * 0x10 再交给模型。payload 只在这次回调里有效，peer_model_submit 内部会拷贝。
 */
static void on_peer_rx(uint8_t msg_type, const uint8_t *payload, uint16_t len, uint8_t seq, void *user)
{
    peer_event_on_rx(msg_type, payload, len, seq, user);
    if (msg_type == PEER_MSG_SNAPSHOT && payload != NULL && len == sizeof(peer_snapshot_t)) {
        peer_snapshot_t snap;
        memcpy(&snap, payload, sizeof(snap));
        peer_model_submit(&snap);
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_level_set("peer_event", ESP_LOG_INFO);
    esp_log_level_set("peer_model", ESP_LOG_INFO);
    if (peer_event_selfcheck() == ESP_OK) {
        static const peer_slave_sink_t sink = {
            .on_rx = on_peer_rx,
            .on_idle = peer_event_on_idle,
            .reply_payload = peer_event_reply_payload,
            .reply_done = peer_event_reply_done,
        };
        (void)peer_slave_start(&sink);
    }
    /* 自检失败时串口不起，麦和模型仍然启动。原来的麦任务也是这样。 */
    peer_model_start();
}
