#include "peer_event.h"
#include "peer_judge.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "peer_event";

/** 长度不对的帧直接丢，不改判定状态。0x11 先排队，等下一帧 0x10 再生效。 */
void peer_event_on_rx(uint8_t msg_type, const uint8_t *payload, uint16_t len, uint8_t seq, void *user)
{
    (void)seq;
    (void)user;
    if (payload == NULL) {
        return;
    }
    if (msg_type == PEER_MSG_ENV_CFG) {
        if (len != sizeof(peer_env_cfg_t)) {
            return;
        }
        peer_env_cfg_t cfg;
        memcpy(&cfg, payload, sizeof(cfg));
        peer_judge_queue_cfg(&cfg);
        return;
    }
    if (msg_type == PEER_MSG_SNAPSHOT) {
        if (len != sizeof(peer_snapshot_t)) {
            return;
        }
        peer_snapshot_t snap;
        memcpy(&snap, payload, sizeof(snap));
        peer_judge_on_snapshot(&snap);
    }
}

/** 空窗只冻结占用。这里不打日志，避免刷屏盖住事件行。 */
void peer_event_on_idle(void *user)
{
    (void)user;
    peer_judge_on_idle();
}

/** 拷贝队头。返回 0 时 tx_event 停止发 0x20。 */
uint16_t peer_event_reply_payload(uint8_t seq, uint8_t *dst, uint16_t dst_max, void *user)
{
    (void)seq;
    (void)user;
    peer_event_t ev;
    if (dst == NULL || dst_max < sizeof(ev) || !peer_judge_peek(&ev)) {
        return 0;
    }
    memcpy(dst, &ev, sizeof(ev));
    return (uint16_t)sizeof(ev);
}

/** 发送成功后出队。这一行是目前唯一保留的业务日志。 */
void peer_event_reply_done(void *user)
{
    (void)user;
    peer_event_t ev;
    if (peer_judge_pop(&ev)) {
        ESP_LOGI(TAG, "事件 %s ch=%s zone=%ld people=%u q=%d", ev.event_type,
                 ev.env_ch[0] ? ev.env_ch : "-", (long)ev.zone_id, ev.people_room,
                 peer_judge_queue_len());
    }
}

esp_err_t peer_event_selfcheck(void)
{
    return peer_judge_selfcheck() ? ESP_OK : ESP_FAIL;
}
