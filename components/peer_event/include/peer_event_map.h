#pragma once

/**
 * 线上 event_type 映射表。编号与主机约定一致：
 *   1 room_entry
 *   2 room_exit
 *   3 zone_exit
 *   4 zone_return
 *   5 environment_threshold_crossed
 *   6 environment_recovered
 *
 * 判定代码只引用编号。回包 ASCII 从本表查出。新增事件：加枚举、加一行表、再写判定分支。
 */

#include "peer_frame.h"

typedef enum {
    PEER_EVT_ID_NONE = 0,
    PEER_EVT_ID_ROOM_ENTRY = 1,
    PEER_EVT_ID_ROOM_EXIT = 2,
    PEER_EVT_ID_ZONE_EXIT = 3,
    PEER_EVT_ID_ZONE_RETURN = 4,
    PEER_EVT_ID_ENV_CROSSED = 5,
    PEER_EVT_ID_ENV_RECOVERED = 6
} peer_evt_id_t;

typedef struct {
    peer_evt_id_t id;
    const char *wire; /**< 写入 0x20 的 event_type，补 0 由调用方做。 */
} peer_evt_map_row_t;

extern const peer_evt_map_row_t peer_evt_map[];
extern const unsigned peer_evt_map_n;

/** 未知 id 返回 NULL。 */
const char *peer_evt_wire_name(peer_evt_id_t id);
