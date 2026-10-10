/**
 * 进出房看 people_room；进出区看每条轨迹的 zone_id（0 不是业务区）。
 * 环境五路各自边沿，回差来自 0x11。
 * 雷达未就绪或主机停发：只冻结占用，环境仍按快照边沿。
 * 同一路已经在坏区时不连报；回到回差带才报恢复。
 * 边沿按帧先后，不用未对时的 utc_unix_s。
 */
#include "peer_judge.h"

#include <string.h>

#define Q_MAX 8   /**< 一拍最多几条事件。满了丢最新的，避免把更早的进出房挤掉。 */
#define CH_N  5

/** 与 k_ch_name、mask 的 bit 顺序一致：0 CO₂、1 温度、2 湿度、3 照度、4 声级。 */
enum {
    CH_CO2 = 0,
    CH_TEMP,
    CH_RH,
    CH_LUX,
    CH_SOUND
};

/** 上一帧里用来对进出区的轨迹摘要，不含坐标。 */
typedef struct {
    uint8_t id;
    int32_t zone;
    char purpose[PEER_STR16];
} trk_t;

/** 写入 0x20 的 env_ch。顺序必须和上面的 enum 一致。 */
static const char *const k_ch_name[CH_N] = {"co2", "temp", "rh", "lux", "sound"};

/**
 * 还没收到 0x11 时用的阈值，对应网页默认：
 * 30℃、湿度 20–80%、CO₂ 1000 ppm、照度 50/1000、声级 60 dB，mask 五路全开。
 * 回差：CO₂ 50、温度 0.5℃、湿度 2%、照度 20、声级 3 dB。
 */
static peer_env_cfg_t default_cfg(void)
{
    peer_env_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.mask = 0x1F;
    c.co2_high_ppm = 1000;
    c.temp_high_x10 = 300;
    c.rh_high_x10 = 800;
    c.rh_low_x10 = 200;
    c.lux_bright = 1000;
    c.lux_dark = 50;
    c.sound_high_x10 = 600;
    c.hyst_co2_ppm = 50;
    c.hyst_temp_x10 = 5;
    c.hyst_rh_x10 = 20;
    c.hyst_lux = 20;
    c.hyst_sound_x10 = 30;
    return c;
}

/** 当前正在用的阈值。 */
static peer_env_cfg_t s_cfg;
/** 已收到、等下一帧 0x10 再生效的阈值。 */
static peer_env_cfg_t s_cfg_wait;
static bool s_cfg_pending;

/** 上一帧轨迹，用来比进出区。 */
static trk_t s_prev[PEER_TARGET_MAX];
static uint8_t s_prev_n;
/** 上一帧 people_room。 */
static uint8_t s_people;
/** 为假时本拍只记占用基线，不报进出。空窗或雷达掉线后会被清掉。 */
static bool s_occ_base;
/** 这一路已经报过越阈、还没报恢复。 */
static bool s_in_bad[CH_N];
/** 雷达未就绪，或主机长时间没发 0x10。只挡住占用事件。 */
static bool s_frozen;

/** 待发 0x20。UART 写完才 pop。 */
static peer_event_t s_q[Q_MAX];
static int s_q_n;

/** 入队。满了丢这一条，不挤掉队头。 */
static void push(const peer_event_t *ev)
{
    if (s_q_n >= Q_MAX) {
        return; /* ponytail: 队列满则丢最新。一周期只回一条，正常排得开。 */
    }
    s_q[s_q_n++] = *ev;
}

/**
 * 拷 ASCII 并保证 dst 以 0 结尾。
 * src_n 是源缓冲容量，遇到 '\\0' 提前停，避免把 purpose 后面的填充也拷进去。
 */
static void copy_ascii(char *dst, size_t dst_n, const char *src, size_t src_n)
{
    memset(dst, 0, dst_n);
    if (src == NULL || dst_n == 0) {
        return;
    }
    size_t lim = src_n;
    if (lim >= dst_n) {
        lim = dst_n - 1;
    }
    for (size_t i = 0; i < lim && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
}

/**
 * 填一条事件。
 * all_targets 为真：房间级，targets[] 带上检测范围内全部目标。
 * 为假：只留 zone_id 相同的目标。
 * env_ch / purpose 传 NULL 时对应字段保持全 0（占用事件）。
 */
static void fill(peer_event_t *ev, peer_evt_id_t id, const char *env_ch, int32_t zone_id,
                 const char *purpose, const peer_snapshot_t *snap, bool all_targets)
{
    memset(ev, 0, sizeof(*ev));
    copy_ascii(ev->event_type, sizeof(ev->event_type), peer_evt_wire_name(id), PEER_EVENT_STR_MAX);
    copy_ascii(ev->env_ch, sizeof(ev->env_ch), env_ch, PEER_STR16);
    ev->zone_id = zone_id;
    copy_ascii(ev->purpose, sizeof(ev->purpose), purpose, PEER_STR16);
    ev->people_room = snap->people_room;
    ev->co2_ppm = snap->co2_ppm;
    ev->temp_c_x10 = snap->temp_c_x10;
    ev->rh_x10 = snap->rh_x10;
    ev->lux = snap->lux;
    ev->sound_db_x10 = snap->sound_db_x10;
    memcpy(ev->room_type, snap->room_type, PEER_STR16);
    ev->room_type[PEER_STR16 - 1] = '\0';

    uint8_t lim = snap->target_n > PEER_TARGET_MAX ? PEER_TARGET_MAX : snap->target_n;
    uint8_t n = 0;
    for (uint8_t i = 0; i < lim; i++) {
        if (!all_targets && snap->targets[i].zone_id != zone_id) {
            continue;
        }
        ev->targets[n].track_id = snap->targets[i].track_id;
        ev->targets[n].x_cm = snap->targets[i].x_cm;
        ev->targets[n].y_cm = snap->targets[i].y_cm;
        n++;
    }
    ev->target_n = n;
}

/** 同一区本拍是否已经记过，避免区内多个人报多条。 */
static bool zone_listed(const int32_t *zs, int n, int32_t z)
{
    for (int i = 0; i < n; i++) {
        if (zs[i] == z) {
            return true;
        }
    }
    return false;
}

/**
 * 占用边沿。调用前调用方已确认 radar_ready!=0。
 * 第一帧（或空窗/雷达恢复后的第一帧）只记住人和区，不报。
 * 进房：上一拍人数 0、这一拍 >0。离房相反。
 * 出区：上一拍某 track 在 zone!=0，这一拍同 id 已不在该区（含人消失）。
 * 进区：这一拍某 track 在 zone!=0，上一拍同 id 不在该区。第一次进区也走 zone_return。
 */
static void on_occupancy(const peer_snapshot_t *snap)
{
    uint8_t lim = snap->target_n > PEER_TARGET_MAX ? PEER_TARGET_MAX : snap->target_n;
    trk_t cur[PEER_TARGET_MAX];
    uint8_t cn = 0;
    for (uint8_t i = 0; i < lim; i++) {
        cur[cn].id = snap->targets[i].track_id;
        cur[cn].zone = snap->targets[i].zone_id;
        memcpy(cur[cn].purpose, snap->targets[i].purpose, PEER_STR16);
        cn++;
    }

    if (!s_occ_base) {
        memcpy(s_prev, cur, sizeof(cur));
        s_prev_n = cn;
        s_people = snap->people_room;
        s_occ_base = true;
        return;
    }

    peer_event_t ev;
    if (s_people == 0 && snap->people_room > 0) {
        fill(&ev, PEER_EVT_ID_ROOM_ENTRY, NULL, 0, NULL, snap, true);
        push(&ev);
    } else if (s_people > 0 && snap->people_room == 0) {
        fill(&ev, PEER_EVT_ID_ROOM_EXIT, NULL, 0, NULL, snap, true);
        push(&ev);
    }

    int32_t left[PEER_TARGET_MAX];
    char left_purpose[PEER_TARGET_MAX][PEER_STR16];
    int nl = 0;
    for (uint8_t i = 0; i < s_prev_n; i++) {
        if (s_prev[i].zone == 0 || zone_listed(left, nl, s_prev[i].zone)) {
            continue;
        }
        bool still = false;
        for (uint8_t j = 0; j < cn; j++) {
            if (cur[j].id == s_prev[i].id && cur[j].zone == s_prev[i].zone) {
                still = true;
                break;
            }
        }
        if (!still) {
            left[nl] = s_prev[i].zone;
            memcpy(left_purpose[nl], s_prev[i].purpose, PEER_STR16);
            nl++;
        }
    }
    for (int i = 0; i < nl; i++) {
        fill(&ev, PEER_EVT_ID_ZONE_EXIT, NULL, left[i], left_purpose[i], snap, false);
        push(&ev);
    }

    int32_t entered[PEER_TARGET_MAX];
    char enter_purpose[PEER_TARGET_MAX][PEER_STR16];
    int ne = 0;
    for (uint8_t j = 0; j < cn; j++) {
        if (cur[j].zone == 0 || zone_listed(entered, ne, cur[j].zone)) {
            continue;
        }
        bool was = false;
        for (uint8_t i = 0; i < s_prev_n; i++) {
            if (s_prev[i].id == cur[j].id && s_prev[i].zone == cur[j].zone) {
                was = true;
                break;
            }
        }
        if (!was) {
            entered[ne] = cur[j].zone;
            memcpy(enter_purpose[ne], cur[j].purpose, PEER_STR16);
            ne++;
        }
    }
    for (int i = 0; i < ne; i++) {
        fill(&ev, PEER_EVT_ID_ZONE_RETURN, NULL, entered[i], enter_purpose[i], snap, false);
        push(&ev);
    }

    memcpy(s_prev, cur, sizeof(cur));
    s_prev_n = cn;
    s_people = snap->people_room;
}

/** 这一路在快照里的读数。失败时快照字段本身就是 -1。 */
static int16_t reading(const peer_snapshot_t *snap, int ch)
{
    switch (ch) {
    case CH_CO2: return snap->co2_ppm;
    case CH_TEMP: return snap->temp_c_x10;
    case CH_RH: return snap->rh_x10;
    case CH_LUX: return snap->lux;
    default: return snap->sound_db_x10;
    }
}

/** mask 里对应这一路的 bit。 */
static uint8_t ch_mask(int ch)
{
    return (uint8_t)(1u << ch);
}

/** 主机把回差发成 0 或负数时用的文档默认值。 */
static int16_t def_hyst(int ch)
{
    switch (ch) {
    case CH_CO2: return 50;
    case CH_TEMP: return 5;
    case CH_RH: return 20;
    case CH_LUX: return 20;
    default: return 30;
    }
}

/** 当前回差。<=0 时换成 def_hyst，避免回差为 0 时越阈和恢复贴在同一点上连报。 */
static int16_t hyst_of(int ch)
{
    int16_t h;
    switch (ch) {
    case CH_CO2: h = s_cfg.hyst_co2_ppm; break;
    case CH_TEMP: h = s_cfg.hyst_temp_x10; break;
    case CH_RH: h = s_cfg.hyst_rh_x10; break;
    case CH_LUX: h = s_cfg.hyst_lux; break;
    default: h = s_cfg.hyst_sound_x10; break;
    }
    if (h <= 0) {
        h = def_hyst(ch);
    }
    return h;
}

/** 上限。CO₂/温度/声级只有这一侧；湿度是上限，照度是过亮。-1 表示不用。 */
static int16_t high_of(int ch)
{
    switch (ch) {
    case CH_CO2: return s_cfg.co2_high_ppm;
    case CH_TEMP: return s_cfg.temp_high_x10;
    case CH_RH: return s_cfg.rh_high_x10;
    case CH_LUX: return s_cfg.lux_bright;
    default: return s_cfg.sound_high_x10;
    }
}

/** 下限。只有湿度和照度有；其它路固定 -1（不用）。 */
static int16_t low_of(int ch)
{
    if (ch == CH_RH) {
        return s_cfg.rh_low_x10;
    }
    if (ch == CH_LUX) {
        return s_cfg.lux_dark;
    }
    return -1;
}

/** mask 没打开的路不参与边沿。 */
static bool enabled(int ch)
{
    return (s_cfg.mask & ch_mask(ch)) != 0;
}

/**
 * 是否越阈。读数 <0（传感器失败）不算越阈。
 * CO₂、温度、声级：v > 上限。等于上限不报。
 * 湿度、照度：低于下限或高于上限。界值 -1 的那一侧不比。
 */
static bool is_bad(int ch, int16_t v)
{
    if (v < 0) {
        return false;
    }
    int16_t hi = high_of(ch);
    int16_t lo = low_of(ch);
    if (ch == CH_RH || ch == CH_LUX) {
        bool bad = false;
        if (lo != -1 && v < lo) {
            bad = true;
        }
        if (hi != -1 && v > hi) {
            bad = true;
        }
        return bad;
    }
    return hi != -1 && v > hi;
}

/**
 * 是否回到恢复带。必须比越阈多退一个回差，避免在阈值上来回抖。
 * 单侧：v <= 上限 - 回差，或 v >= 下限 + 回差。
 * 湿度/照度双侧：落在 [下限+回差, 上限-回差]。上下限贴太近时把回差收到区间的一半，否则永远恢复不了。
 */
static bool is_recovered(int ch, int16_t v)
{
    if (v < 0) {
        return false;
    }
    int16_t h = hyst_of(ch);
    int16_t hi = high_of(ch);
    int16_t lo = low_of(ch);
    if (ch == CH_RH || ch == CH_LUX) {
        /* 恢复带 [lo+h, hi-h]；带宽不够时把回差收到还能夹出一段。 */
        if (lo == -1 && hi == -1) {
            return false;
        }
        if (lo == -1) {
            return v <= (int16_t)(hi - h);
        }
        if (hi == -1) {
            return v >= (int16_t)(lo + h);
        }
        int span = (int)hi - (int)lo;
        if (span < 2) {
            return false;
        }
        if ((int)h * 2 > span) {
            h = (int16_t)(span / 2);
            if (h < 1) {
                h = 1;
            }
        }
        return v >= (int16_t)(lo + h) && v <= (int16_t)(hi - h);
    }
    if (hi == -1) {
        return false;
    }
    return v <= (int16_t)(hi - h);
}

/**
 * 环境边沿。没有“第一帧只建基线”：开机时已经超阈也会报一次。
 * mask 关掉的路清掉坏区标记，重新打开后若仍超阈会再报。
 * 读数 -1 保持上一状态，本拍不报。
 * 坏区里但还没回到恢复带：保持 in_bad，不连报。
 */
static void on_env(const peer_snapshot_t *snap)
{
    for (int ch = 0; ch < CH_N; ch++) {
        if (!enabled(ch)) {
            s_in_bad[ch] = false;
            continue;
        }
        int16_t v = reading(snap, ch);
        if (v < 0) {
            continue; /* 本拍无合法读数，保持上一状态，不边沿。 */
        }
        bool now_bad = is_bad(ch, v);
        peer_event_t ev;
        if (!s_in_bad[ch] && now_bad) {
            fill(&ev, PEER_EVT_ID_ENV_CROSSED, k_ch_name[ch], 0, NULL, snap, true);
            push(&ev);
            s_in_bad[ch] = true;
        } else if (s_in_bad[ch] && is_recovered(ch, v)) {
            fill(&ev, PEER_EVT_ID_ENV_RECOVERED, k_ch_name[ch], 0, NULL, snap, true);
            push(&ev);
            s_in_bad[ch] = false;
        }
        /* 仍在坏区但未回到恢复带：保持 in_bad，不连报。 */
    }
}

/** 回到上电状态：默认阈值、无基线、队列空。 */
void peer_judge_reset(void)
{
    s_cfg = default_cfg();
    memset(&s_cfg_wait, 0, sizeof(s_cfg_wait));
    s_cfg_pending = false;
    memset(s_prev, 0, sizeof(s_prev));
    s_prev_n = 0;
    s_people = 0;
    s_occ_base = false;
    memset(s_in_bad, 0, sizeof(s_in_bad));
    s_frozen = false;
    s_q_n = 0;
}

/** 只暂存。和当前阈值相同也会再记一次，下一帧快照仍用新规则比边沿。 */
void peer_judge_queue_cfg(const peer_env_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    s_cfg_wait = *cfg;
    s_cfg_pending = true;
}

/** 主机停发。下一帧合法 0x10 的占用部分当新基线，不因人数变 0 报离房。 */
void peer_judge_on_idle(void)
{
    s_frozen = true;
    s_occ_base = false;
}

/**
 * 一帧快照。
 * 若有待生效的 0x11，本拍起用新阈值，不把环境坏区清掉，所以阈值变严时已经超温会立刻报。
 * radar_ready==0：冻结占用并跳过 on_occupancy，环境照常走 on_env。
 * 从冻结恢复的第一帧只记占用基线。
 */
void peer_judge_on_snapshot(const peer_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }
    if (s_cfg_pending) {
        s_cfg = s_cfg_wait;
        s_cfg_pending = false;
    }

    if (!snap->radar_ready) {
        s_frozen = true;
        s_occ_base = false;
    } else {
        if (s_frozen) {
            s_frozen = false;
            s_occ_base = false;
        }
        on_occupancy(snap);
    }
    on_env(snap);
}

/** 看队头，给组 0x20 用。 */
bool peer_judge_peek(peer_event_t *out)
{
    if (out == NULL || s_q_n == 0) {
        return false;
    }
    *out = s_q[0];
    return true;
}

/** 0x20 已写出后出队。后面的事件前移。 */
bool peer_judge_pop(peer_event_t *out)
{
    if (out == NULL || s_q_n == 0) {
        return false;
    }
    *out = s_q[0];
    memmove(&s_q[0], &s_q[1], (size_t)(s_q_n - 1) * sizeof(s_q[0]));
    s_q_n--;
    return true;
}

int peer_judge_queue_len(void)
{
    return s_q_n;
}

static peer_snapshot_t blank(void)
{
    peer_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.radar_ready = 1;
    s.co2_ppm = 400;
    s.temp_c_x10 = 250;
    s.rh_x10 = 500;
    s.lux = 200;
    s.sound_db_x10 = 400;
    memcpy(s.room_type, "living", 6);
    return s;
}

/**
 * 主机可跑的自检：结构体尺寸、事件名、Modbus CRC 向量、进出房/区、
 * CO₂ 与温度边沿、0x11 生效时机、空窗不报离房、流式拆帧。
 * 任何一步不对就返回 false。不依赖 ESP-IDF。
 */
bool peer_judge_selfcheck(void)
{
    if (sizeof(peer_snapshot_t) != 393 || sizeof(peer_event_t) != 136 ||
        sizeof(peer_env_cfg_t) != 26) {
        return false;
    }
    /* 1–6 是边沿，7–12 是模型类名。条数变了说明表和枚举没一起改。 */
    if (peer_evt_map_n != 12 ||
        peer_evt_wire_name(PEER_EVT_ID_ROOM_ENTRY) == NULL ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_ROOM_ENTRY), "room_entry") != 0) {
        return false;
    }
    static const uint8_t modbus[] = "123456789";
    if (peer_crc16(modbus, 9) != 0x4B37) {
        return false;
    }

    uint8_t frame[64];
    size_t n = peer_frame_encode(PEER_MSG_EVENT, 3, NULL, 0, frame, sizeof(frame));
    uint8_t msg = 0, seq = 0;
    const uint8_t *payload = NULL;
    uint16_t plen = 0xFFFF;
    if (n != 9 || !peer_frame_decode(frame, n, &msg, &seq, &payload, &plen) ||
        msg != PEER_MSG_EVENT || seq != 3 || plen != 0) {
        return false;
    }

    peer_judge_reset();
    peer_event_t ev;
    peer_snapshot_t s = blank();
    peer_judge_on_snapshot(&s);
    if (peer_judge_pop(&ev)) {
        return false;
    }

    s.people_room = 1;
    s.target_n = 2;
    s.targets[0].track_id = 1;
    s.targets[0].x_cm = 120;
    s.targets[0].y_cm = 80;
    s.targets[0].zone_id = 3;
    memcpy(s.targets[0].purpose, "bed", 3);
    s.targets[1].track_id = 2;
    s.targets[1].zone_id = 0;
    s.targets[1].x_cm = 10;
    peer_judge_on_snapshot(&s);
    if (!peer_judge_pop(&ev) || strcmp(ev.event_type, peer_evt_wire_name(PEER_EVT_ID_ROOM_ENTRY)) != 0 ||
        ev.zone_id != 0 || ev.env_ch[0] != '\0' || ev.people_room != 1 || ev.target_n != 2) {
        return false;
    }
    if (!peer_judge_pop(&ev) || strcmp(ev.event_type, peer_evt_wire_name(PEER_EVT_ID_ZONE_RETURN)) != 0 ||
        ev.zone_id != 3 || strcmp(ev.purpose, "bed") != 0 || ev.target_n != 1 ||
        ev.targets[0].track_id != 1) {
        return false;
    }

    peer_judge_on_snapshot(&s);
    if (peer_judge_pop(&ev)) {
        return false;
    }

    s = blank();
    peer_judge_on_snapshot(&s);
    if (!peer_judge_pop(&ev) || strcmp(ev.event_type, peer_evt_wire_name(PEER_EVT_ID_ROOM_EXIT)) != 0) {
        return false;
    }
    if (!peer_judge_pop(&ev) || strcmp(ev.event_type, peer_evt_wire_name(PEER_EVT_ID_ZONE_EXIT)) != 0 || ev.zone_id != 3) {
        return false;
    }

    s.co2_ppm = 1100;
    peer_judge_on_snapshot(&s);
    if (!peer_judge_pop(&ev) || strcmp(ev.event_type, peer_evt_wire_name(PEER_EVT_ID_ENV_CROSSED)) != 0 ||
        strcmp(ev.env_ch, "co2") != 0 || ev.co2_ppm != 1100) {
        return false;
    }
    s.co2_ppm = 1090;
    peer_judge_on_snapshot(&s);
    if (peer_judge_pop(&ev)) {
        return false;
    }
    s.co2_ppm = 900;
    peer_judge_on_snapshot(&s);
    if (!peer_judge_pop(&ev) || strcmp(ev.event_type, peer_evt_wire_name(PEER_EVT_ID_ENV_RECOVERED)) != 0 ||
        strcmp(ev.env_ch, "co2") != 0) {
        return false;
    }

    /* 雷达掉线：不报离房。 */
    s.people_room = 1;
    s.target_n = 1;
    s.targets[0].track_id = 9;
    s.targets[0].zone_id = 4;
    memcpy(s.targets[0].purpose, "sofa", 4);
    peer_judge_on_snapshot(&s);
    while (peer_judge_pop(&ev)) {
    }
    s.radar_ready = 0;
    s.people_room = 0;
    s.target_n = 0;
    peer_judge_on_snapshot(&s);
    if (peer_judge_pop(&ev)) {
        return false;
    }
    s.radar_ready = 1;
    s.people_room = 0;
    peer_judge_on_snapshot(&s);
    if (peer_judge_pop(&ev)) {
        return false;
    }

    peer_env_cfg_t cfg = default_cfg();
    cfg.co2_high_ppm = 350;
    peer_judge_queue_cfg(&cfg);
    s = blank();
    s.co2_ppm = 200;
    peer_judge_on_snapshot(&s);
    if (peer_judge_pop(&ev)) {
        return false;
    }
    s.co2_ppm = 400;
    peer_judge_on_snapshot(&s);
    if (!peer_judge_pop(&ev) || strcmp(ev.env_ch, "co2") != 0 ||
        strcmp(ev.event_type, peer_evt_wire_name(PEER_EVT_ID_ENV_CROSSED)) != 0) {
        return false;
    }

    /* 温度 >30℃ 应报；已超温、空窗、雷达未就绪、重复 0x11 都不得把边沿吃掉。 */
    peer_judge_reset();
    s = blank();
    peer_judge_on_snapshot(&s);
    while (peer_judge_pop(&ev)) {
    }
    s.temp_c_x10 = 310;
    peer_judge_on_snapshot(&s);
    if (!peer_judge_pop(&ev) || strcmp(ev.env_ch, "temp") != 0 ||
        strcmp(ev.event_type, peer_evt_wire_name(PEER_EVT_ID_ENV_CROSSED)) != 0) {
        return false;
    }

    peer_judge_reset();
    s = blank();
    s.temp_c_x10 = 310;
    peer_judge_on_snapshot(&s);
    if (!peer_judge_pop(&ev) || strcmp(ev.env_ch, "temp") != 0) {
        return false;
    }

    peer_judge_reset();
    s = blank();
    peer_judge_on_snapshot(&s);
    while (peer_judge_pop(&ev)) {
    }
    cfg = default_cfg();
    peer_judge_queue_cfg(&cfg);
    s.temp_c_x10 = 310;
    peer_judge_on_snapshot(&s);
    if (!peer_judge_pop(&ev) || strcmp(ev.env_ch, "temp") != 0) {
        return false;
    }

    peer_judge_reset();
    s = blank();
    peer_judge_on_snapshot(&s);
    while (peer_judge_pop(&ev)) {
    }
    peer_judge_on_idle();
    s.temp_c_x10 = 310;
    peer_judge_on_snapshot(&s);
    if (!peer_judge_pop(&ev) || strcmp(ev.env_ch, "temp") != 0) {
        return false;
    }

    peer_judge_reset();
    s = blank();
    peer_judge_on_snapshot(&s);
    while (peer_judge_pop(&ev)) {
    }
    s.radar_ready = 0;
    s.temp_c_x10 = 310;
    peer_judge_on_snapshot(&s);
    if (!peer_judge_pop(&ev) || strcmp(ev.env_ch, "temp") != 0) {
        return false;
    }

    peer_env_cfg_t packed;
    memset(&packed, 0, sizeof(packed));
    n = peer_frame_encode(PEER_MSG_ENV_CFG, 1, (const uint8_t *)&packed, sizeof(packed),
                          frame, sizeof(frame));
    if (n != 9u + sizeof(peer_env_cfg_t) ||
        !peer_frame_decode(frame, n, &msg, &seq, &payload, &plen) ||
        msg != PEER_MSG_ENV_CFG || plen != sizeof(peer_env_cfg_t)) {
        return false;
    }

    /* 流式拆帧：前缀垃圾、CRC 失败丢 1 字节后再对齐。 */
    uint8_t stream[80];
    memset(stream, 0x11, 3);
    n = peer_frame_encode(PEER_MSG_ENV_CFG, 7, (const uint8_t *)&packed, sizeof(packed),
                          stream + 3, sizeof(stream) - 3);
    size_t drop = 99;
    if (!peer_frame_next(stream, 3 + n, &drop, &msg, &seq, &payload, &plen) || drop != 3 ||
        msg != PEER_MSG_ENV_CFG || seq != 7 || plen != sizeof(peer_env_cfg_t)) {
        return false;
    }
    uint8_t bad[16];
    n = peer_frame_encode(PEER_MSG_EVENT, 1, NULL, 0, bad, sizeof(bad));
    bad[n - 1] ^= 0xFF;
    memcpy(stream, bad, n);
    n += peer_frame_encode(PEER_MSG_EVENT, 2, NULL, 0, stream + n, sizeof(stream) - n);
    drop = 0;
    if (peer_frame_next(stream, n, &drop, &msg, &seq, &payload, &plen)) {
        return false;
    }
    if (drop != 1) {
        return false;
    }
    memmove(stream, stream + drop, n - drop);
    n -= drop;
    if (!peer_frame_next(stream, n, &drop, &msg, &seq, &payload, &plen) || drop != 8 ||
        msg != PEER_MSG_EVENT || seq != 2) {
        return false;
    }

    peer_judge_reset();
    s = blank();
    s.people_room = 1;
    peer_judge_on_snapshot(&s);
    while (peer_judge_pop(&ev)) {
    }
    peer_judge_on_idle();
    s.people_room = 0;
    peer_judge_on_snapshot(&s);
    if (peer_judge_pop(&ev)) {
        return false;
    }
    /* 模型 6 类只在第一名变化时入队，名字必须和映射表一致。 */
    if (strcmp(peer_evt_wire_name(PEER_EVT_ID_RESTING), "resting") != 0 ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_SLEEP), "sleep") != 0 ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_RESTING_BED), "resting_bed") != 0 ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_SLEEP_IN_NIGHT_BED), "sleep_in_night_bed") != 0 ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_WAKE_UP_IN_NIGHT_BED), "wake_up_in_night_bed") != 0 ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_EMPTY_BED), "empty_bed") != 0) {
        return false;
    }
    peer_judge_reset();
    return true;
}
