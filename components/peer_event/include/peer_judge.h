#pragma once

/**
 * 边沿判定给 peer_event 用。
 * 模型任务只通过 peer_judge_push 把一条 0x20 放进同一队列。
 * 线上英文名一律从 peer_event_map 查，这里不要写死 "room_entry" 这类字符串。
 *
 * 占用：people_room 从 0 变正数报进房，从正数变 0 报离房。
 *       zone_id!=0 且同一 track 相对上一帧离开/进入该区时报 zone_exit / zone_return（含第一次进区）。
 *       雷达未就绪或空窗期间不报进出，恢复后的第一帧只作新基线。
 * 环境：五路各自比较 0x11。越阈只报一次，回到回差带再报恢复。
 *       开机已经超阈、重复 0x11、空窗、雷达掉线，都不会把环境边沿清掉。
 * 0x11 在下一帧 0x10 里才换成新阈值，不要求对端应答。
 */

#include "peer_event_map.h"
#include "peer_frame.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 清空轨迹、环境坏区标记和事件队列，阈值回到文档默认值。 */
void peer_judge_reset(void);

/** 暂存一帧 0x11。真正换阈值发生在下一次 on_snapshot。 */
void peer_judge_queue_cfg(const peer_env_cfg_t *cfg);

/** 吃一帧快照：先换待生效配置，再判占用，再判环境。 */
void peer_judge_on_snapshot(const peer_snapshot_t *snap);

/** 主机停发。清占用基线，不改环境坏区标记。 */
void peer_judge_on_idle(void);

/** 看队头，不出队。空队列返回 false。 */
bool peer_judge_peek(peer_event_t *out);

/** 弹出队头。空队列返回 false。 */
bool peer_judge_pop(peer_event_t *out);

/**
 * 模型任务入队一条已经填好的事件。
 * 相同 event_type、zone_id、env_ch 距上次成功入队不满 30 秒：丢掉并返回 true。
 * 队列已满返回 false，不记冷却时间，队头保持不变。ev 为空返回 false。
 * ev 只在本次调用内读取。
 */
bool peer_judge_push(const peer_event_t *ev);

/** 队列里还没发出去的事件条数。 */
int peer_judge_queue_len(void);

/** 尺寸、CRC 向量、进出房/区、环境边沿、流式拆帧。失败返回 false。 */
bool peer_judge_selfcheck(void);

#ifdef __cplusplus
}
#endif
