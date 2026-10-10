#pragma once

/**
 * 把 0x10 收成每区 10 秒雷达和全屋 10 秒环境。
 * 不跑神经网络，不入队。模型任务喂快照；on_snapshot 返回 true 时立刻 zones()，
 * 用返回的输入做这一拍。推理期间不要再喂快照，否则这一拍会混进新秒。
 */

#include "peer_frame.h"

#include <stdbool.h>
#include <stdint.h>

/** 一次推理里，一个已经满 10 秒的区。环境行和 epoch 各区相同。 */
typedef struct {
    int32_t zone_id;
    int model_zone;                 /**< purpose 映射，0–6。认不出为 5。 */
    int room;                       /**< room_type 映射，0–6。认不出为 6。 */
    double epoch_sec;               /**< 这 10 行里最旧一秒的 utc_unix_s。 */
    float radar[50];                /**< 旧秒在前。列：静止、微动、慢动、快动、事件码。 */
    float env[50];                  /**< 列：温度℃、湿度%、CO₂ ppm、照度、声级 dB。 */
    char purpose[PEER_STR16];
} peer_model_zone_in_t;

/** 给 PC 自检查已提交的雷达，不满 10 秒时后面的行无意义。 */
typedef struct {
    int filled;                     /**< 已收口秒数，0–10。 */
    int model_zone;
    int last_class;                 /**< 已上报的类，0–5。还没报过为 -1。 */
    float radar[50];
} peer_model_zone_info_t;

/** 6 类概率里最高的下标。相同则保留更前面的类，避免来回跳。 */
int peer_model_top_class(const float prob[6]);

/** 上电状态：槽位释放，环境有效值也清掉。测试在两组场景之间调用。 */
void peer_model_window_reset(void);

/**
 * 快照队列溢出或调用方要求作废。
 * 清已提交行、当前秒、上一拍类别和 2 秒计数。槽位和环境有效值保留。
 */
void peer_model_window_invalidate(void);

/**
 * 吃一帧快照。utc 加 1 才收口上一秒。
 * 返回 true：这一下刚好凑满第一次 10 秒，或之后又收口了 2 秒。调用方应马上 zones()。
 */
bool peer_model_window_on_snapshot(const peer_snapshot_t *snap);

/**
 * 取走这一拍里 filled 已到 10 的区，按 zone_id 从小到大。并清掉触发标志。
 * 返回写入 out 的个数。max 不够时只写前 max 个。
 */
int peer_model_window_zones(peer_model_zone_in_t *out, int max);

/** 找不到这个区返回 false。 */
bool peer_model_window_info(int32_t zone_id, peer_model_zone_info_t *out);

/**
 * 第一名相对上一拍变了才填 ev 并返回 true。不修改上一拍类别。
 * class_index 为 0–5，对应映射表 7–12。
 */
bool peer_model_window_event(int32_t zone_id, int class_index, peer_event_t *ev);

/** 只有 peer_judge_push 成功之后才调用。入队失败不要调用，下一拍还会再试。 */
void peer_model_window_event_sent(int32_t zone_id, int class_index);
