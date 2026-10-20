#include "peer_event_map.h"

/**
 * 0x20 的 event_type 只从这张表取。
 * 编号已经发给主机的不要改、不要复用。新增事件在表尾加一行，并在判定里加分支。
 * 当前模型第一名是 resting_bed、sleep_in_night_bed、work_office，只在类名变化时入队。
 */
const peer_evt_map_row_t peer_evt_map[] = {
    {PEER_EVT_ID_ROOM_ENTRY, "room_entry"},
    {PEER_EVT_ID_ROOM_EXIT, "room_exit"},
    {PEER_EVT_ID_ZONE_EXIT, "zone_exit"},
    {PEER_EVT_ID_ZONE_RETURN, "zone_return"},
    {PEER_EVT_ID_ENV_CROSSED, "environment_threshold_crossed"},
    {PEER_EVT_ID_ENV_RECOVERED, "environment_recovered"},
    {PEER_EVT_ID_RESTING, "resting"},
    {PEER_EVT_ID_SLEEP, "sleep"},
    {PEER_EVT_ID_RESTING_BED, "resting_bed"},
    {PEER_EVT_ID_SLEEP_IN_NIGHT_BED, "sleep_in_night_bed"},
    {PEER_EVT_ID_WAKE_UP_IN_NIGHT_BED, "wake_up_in_night_bed"},
    {PEER_EVT_ID_EMPTY_BED, "empty_bed"},
    {PEER_EVT_ID_WORK_OFFICE, "work_office"},
};

const unsigned peer_evt_map_n = sizeof(peer_evt_map) / sizeof(peer_evt_map[0]);

/** 按 id 查英文名。表里没有则返回 NULL，调用方应把 event_type 留空。 */
const char *peer_evt_wire_name(peer_evt_id_t id)
{
    for (unsigned i = 0; i < peer_evt_map_n; i++) {
        if (peer_evt_map[i].id == id) {
            return peer_evt_map[i].wire;
        }
    }
    return NULL;
}
