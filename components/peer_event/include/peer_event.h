#pragma once

/**
 * 事件组件：校验 0x10/0x11 长度，交给 peer_judge 排队，再把队头交给 UART 组 0x20。
 * 不打开串口。main 把本文件的四个函数填进 peer_slave_sink_t。
 * 以后若还要把快照分给别的模块，在 main 的 on_rx 里再拷一份即可，不要从这里直接调 UART。
 */

#include "esp_err.h"
#include "peer_frame.h"

#include <stdint.h>

/**
 * 合法帧入口。
 * 0x11 长度必须是 sizeof(peer_env_cfg_t)（26），否则丢掉。
 * 0x10 长度必须是 sizeof(peer_snapshot_t)（393），旧的短帧同样丢掉。
 * payload 只在本次调用内有效。
 */
void peer_event_on_rx(uint8_t msg_type, const uint8_t *payload, uint16_t len, uint8_t seq, void *user);

/** 主机长时间不发 0x10。只冻结占用边沿，环境通道仍按下一帧快照判定。 */
void peer_event_on_idle(void *user);

/**
 * 把队头拷进 dst，类型是 peer_event_t。
 * 无事件或 dst 放不下时返回 0。只看不弹，避免 UART 还没写完就把事件丢掉。
 */
uint16_t peer_event_reply_payload(uint8_t seq, uint8_t *dst, uint16_t dst_max, void *user);

/** 一帧 0x20 写完后出队，并打一行事件日志。 */
void peer_event_reply_done(void *user);

/** 结构体尺寸、CRC、边沿规则的主机侧自检。失败返回 ESP_FAIL。 */
esp_err_t peer_event_selfcheck(void);
