#pragma once

/**
 * 融合推理运行时。
 * submit 只在串口回调里调用，内部会拷贝快照。
 * 推理在本组件自己的任务里做，不写 UART。
 */

#include "peer_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建快照队列、左声道环、麦任务和模型任务。重复调用直接返回。 */
void peer_model_start(void);

/**
 * 拷贝一帧 0x10 后返回。
 * 队列满则丢掉这一帧，并作废传感器窗口，不挤掉队列里已有的快照。
 */
void peer_model_submit(const peer_snapshot_t *snap);

#ifdef __cplusplus
}
#endif
