#include "peer_model_window.h"

#include "peer_event_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

#define ZONE_MAX 16
#define ROWS     10
#define COLS     5

/**
 * 一个业务区的窗口。
 * 当前秒的轨迹和「见过进出」在收口前会反复改；out_* 只在收口时从最后一帧拷走，
 * 避免下一秒的第一帧把还没读走的事件目标盖掉。
 */
typedef struct {
    int32_t zone_id;
    char purpose[PEER_STR16];       /**< 最近一次收口用的用途。 */
    int model_zone;
    int last_class;                 /**< -1 表示还没上报过。 */
    int filled;
    float radar[ROWS * COLS];
    uint8_t prev_ids[PEER_TARGET_MAX];
    uint8_t prev_n;
    uint8_t cur_ids[PEER_TARGET_MAX];
    uint8_t cur_n;
    float counts[4];
    bool saw_enter;
    bool saw_leave;
    bool seen_frame;                /**< 这一秒已经比过至少一帧。 */
    bool purpose_set;               /**< 这一秒最后一帧有可计数的人。 */
    char purpose_now[PEER_STR16];
    uint8_t stage_n;
    peer_event_target_t stage[PEER_TARGET_MAX];
    uint8_t out_n;                  /**< 收口那一帧的可计数目标，给 0x20 用。 */
    peer_event_target_t out_targets[PEER_TARGET_MAX];
} zone_t;

static zone_t *s_zone[ZONE_MAX];
static int32_t s_fail_id[ZONE_MAX];
static int s_fail_n;
static float s_env[ROWS * COLS];
static uint32_t s_row_utc[ROWS];
static int s_env_filled;
static bool s_hold_ok[5];
static float s_hold[5];
static int s_room;
static peer_snapshot_t s_committed;
static bool s_have_open;
static uint32_t s_open_utc;
static peer_snapshot_t s_open_snap;
static bool s_fired_first;
static int s_since;
static bool s_beat;

static const char *const k_purpose[] = {
    "bed1", "bed2", "sofa", "desk", "cooking", "generic", "whole_room"
};
static const char *const k_room[] = {
    "bedroom", "living", "kitchen", "work", "bathroom", "passage", "generic"
};

/** 设备上放 PSRAM。失败返回 NULL，调用方把这个区记成以后忽略。 */
static zone_t *zone_alloc(void)
{
#if defined(ESP_PLATFORM)
    return heap_caps_calloc(1, sizeof(zone_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(1, sizeof(zone_t));
#endif
}

static void zone_free(zone_t *z)
{
#if defined(ESP_PLATFORM)
    heap_caps_free(z);
#else
    free(z);
#endif
}

/** 16 字节字段拷成 C 字符串。没有 '\\0' 时在第 17 字节补上。 */
static void field_str(char dst[PEER_STR16 + 1], const char src[PEER_STR16])
{
    memcpy(dst, src, PEER_STR16);
    dst[PEER_STR16] = '\0';
}

static int map_exact(const char *s, const char *const *names, int n, int fallback)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(s, names[i]) == 0) {
            return i;
        }
    }
    return fallback;
}

/** 0 和大于 100 返回 -1，不参与人数，也不参与进出。 */
static int kinesia_bin(uint8_t k)
{
    if (k == 1) {
        return 0;
    }
    if (k >= 2 && k <= 30) {
        return 1;
    }
    if (k >= 31 && k <= 60) {
        return 2;
    }
    if (k >= 61 && k <= 100) {
        return 3;
    }
    return -1;
}

static bool id_in(const uint8_t *ids, uint8_t n, uint8_t id)
{
    for (uint8_t i = 0; i < n; i++) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

static bool failed_id(int32_t zone_id)
{
    for (int i = 0; i < s_fail_n; i++) {
        if (s_fail_id[i] == zone_id) {
            return true;
        }
    }
    return false;
}

static zone_t *find_zone(int32_t zone_id)
{
    for (int i = 0; i < ZONE_MAX; i++) {
        if (s_zone[i] != NULL && s_zone[i]->zone_id == zone_id) {
            return s_zone[i];
        }
    }
    return NULL;
}

#if defined(ESP_PLATFORM)
/** 只在某个 zone_id 第一次建槽时打。用来看主机有没有在未配置时仍下发非 0 区号。 */
static void log_zone_create(const peer_snapshot_t *snap, int index)
{
    const peer_target_t *t = &snap->targets[index];
    char purpose[PEER_STR16 + 1];
    char special[PEER_STR16 + 1];
    char purpose_hex[PEER_STR16 * 2 + 1];
    field_str(purpose, t->purpose);
    field_str(special, t->special);
    for (int i = 0; i < PEER_STR16; i++) {
        sprintf(purpose_hex + i * 2, "%02x", (unsigned char)t->purpose[i]);
    }
    purpose_hex[PEER_STR16 * 2] = '\0';
    ESP_LOGI("peer_model",
             "new zone utc=%lu target_n=%u idx=%d zone_id=%ld track=%u kinesia=%u purpose=\"%s\" special=\"%s\" purpose_hex=%s",
             (unsigned long)snap->utc_unix_s, snap->target_n, index, (long)t->zone_id, t->track_id,
             t->kinesia, purpose, special, purpose_hex);
}
#endif

/** 申请失败的区记下来，后面的快照不再试。表满了也直接忽略。 */
static zone_t *ensure_zone(int32_t zone_id)
{
    zone_t *z = find_zone(zone_id);
    if (z != NULL || failed_id(zone_id)) {
        return z;
    }
    int slot = -1;
    for (int i = 0; i < ZONE_MAX; i++) {
        if (s_zone[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return NULL;
    }
    z = zone_alloc();
    if (z == NULL) {
        if (s_fail_n < ZONE_MAX) {
            s_fail_id[s_fail_n++] = zone_id;
        }
        return NULL;
    }
    z->zone_id = zone_id;
    z->last_class = -1;
    z->model_zone = 5;
    s_zone[slot] = z;
    return z;
}

/** 清已提交窗口。槽位和环境有效值不动。 */
static void wipe(void)
{
    for (int i = 0; i < ZONE_MAX; i++) {
        zone_t *z = s_zone[i];
        if (z == NULL) {
            continue;
        }
        z->filled = 0;
        z->last_class = -1;
        z->prev_n = 0;
        z->cur_n = 0;
        z->saw_enter = false;
        z->saw_leave = false;
        z->seen_frame = false;
        z->purpose_set = false;
        z->stage_n = 0;
        z->out_n = 0;
        memset(z->radar, 0, sizeof(z->radar));
        memset(z->counts, 0, sizeof(z->counts));
    }
    s_env_filled = 0;
    s_fired_first = false;
    s_since = 0;
    s_beat = false;
    s_have_open = false;
}

static void slide_in(float *dst, uint32_t *utc, int *filled, const float row[COLS], uint32_t sec, bool keep_utc)
{
    if (*filled < ROWS) {
        memcpy(dst + (*filled) * COLS, row, COLS * sizeof(float));
        if (keep_utc && utc != NULL) {
            utc[*filled] = sec;
        }
        (*filled)++;
        return;
    }
    memmove(dst, dst + COLS, (ROWS - 1) * COLS * sizeof(float));
    memcpy(dst + (ROWS - 1) * COLS, row, COLS * sizeof(float));
    if (keep_utc && utc != NULL) {
        memmove(utc, utc + 1, (ROWS - 1) * sizeof(uint32_t));
        utc[ROWS - 1] = sec;
    }
}

/**
 * 最后一帧某路不是 -1 就记下，哪怕这一秒因为别的路从未有效而整窗清空。
 * 五路都有过有效值才允许收口。
 */
static bool env_row(const peer_snapshot_t *s, float row[COLS])
{
    const int16_t raw[COLS] = {
        s->temp_c_x10, s->rh_x10, s->co2_ppm, s->lux, s->sound_db_x10
    };
    const bool scale[COLS] = {true, true, false, false, true};
    for (int i = 0; i < COLS; i++) {
        if (raw[i] != -1) {
            s_hold[i] = scale[i] ? (float)raw[i] / 10.0f : (float)raw[i];
            s_hold_ok[i] = true;
        }
    }
    for (int i = 0; i < COLS; i++) {
        if (!s_hold_ok[i]) {
            return false;
        }
        row[i] = s_hold[i];
    }
    return true;
}

/** 这一帧里该区可计数的人。人数和目标都只反映这一帧。 */
static void frame_people(const peer_snapshot_t *snap, int32_t zone_id, uint8_t *ids, uint8_t *n,
                         float counts[4], peer_event_target_t *targets, uint8_t *tn, char *purpose,
                         bool *have_purpose)
{
    memset(counts, 0, 4 * sizeof(float));
    *n = 0;
    *tn = 0;
    *have_purpose = false;
    uint8_t lim = snap->target_n > PEER_TARGET_MAX ? PEER_TARGET_MAX : snap->target_n;
    for (uint8_t i = 0; i < lim; i++) {
        const peer_target_t *t = &snap->targets[i];
        if (t->zone_id != zone_id) {
            continue;
        }
        int bin = kinesia_bin(t->kinesia);
        if (bin < 0) {
            continue;
        }
        counts[bin] += 1.0f;
        if (*n < PEER_TARGET_MAX) {
            ids[(*n)++] = t->track_id;
        }
        if (*tn < PEER_TARGET_MAX) {
            targets[*tn].track_id = t->track_id;
            peer_motion_name(t->kinesia, targets[*tn].motion);
            targets[*tn].x_cm = t->x_cm;
            targets[*tn].y_cm = t->y_cm;
            (*tn)++;
        }
        if (!*have_purpose) {
            field_str(purpose, t->purpose);
            *have_purpose = true;
        }
    }
}

static void apply_zone(zone_t *z, const peer_snapshot_t *snap)
{
    uint8_t ids[PEER_TARGET_MAX];
    uint8_t n = 0;
    uint8_t tn = 0;
    float counts[4];
    peer_event_target_t targets[PEER_TARGET_MAX];
    char purpose[PEER_STR16 + 1];
    bool have_purpose = false;
    frame_people(snap, z->zone_id, ids, &n, counts, targets, &tn, purpose, &have_purpose);

    /* 秒的第一帧跟上一秒收口时的集合比，之后跟本秒上一帧比。 */
    const uint8_t *base = z->seen_frame ? z->cur_ids : z->prev_ids;
    uint8_t base_n = z->seen_frame ? z->cur_n : z->prev_n;
    for (uint8_t i = 0; i < n; i++) {
        if (!id_in(base, base_n, ids[i])) {
            z->saw_enter = true;
        }
    }
    for (uint8_t i = 0; i < base_n; i++) {
        if (!id_in(ids, n, base[i])) {
            z->saw_leave = true;
        }
    }
    memcpy(z->cur_ids, ids, n);
    z->cur_n = n;
    memcpy(z->counts, counts, sizeof(counts));
    z->seen_frame = true;
    memcpy(z->stage, targets, tn * sizeof(targets[0]));
    z->stage_n = tn;
    if (have_purpose) {
        memcpy(z->purpose_now, purpose, PEER_STR16);
        z->purpose_now[PEER_STR16 - 1] = '\0';
        z->purpose_set = true;
    }
}

static void open_zones(const peer_snapshot_t *snap)
{
    uint8_t lim = snap->target_n > PEER_TARGET_MAX ? PEER_TARGET_MAX : snap->target_n;
    for (uint8_t i = 0; i < lim; i++) {
        int32_t id = snap->targets[i].zone_id;
        if (id != 0) {
#if defined(ESP_PLATFORM)
            if (find_zone(id) == NULL && !failed_id(id)) {
                log_zone_create(snap, i);
            }
#endif
            ensure_zone(id);
        }
    }
    for (int i = 0; i < ZONE_MAX; i++) {
        if (s_zone[i] != NULL) {
            apply_zone(s_zone[i], snap);
        }
    }
}

static void begin_open(const peer_snapshot_t *snap)
{
    s_have_open = true;
    s_open_utc = snap->utc_unix_s;
    s_open_snap = *snap;
    for (int i = 0; i < ZONE_MAX; i++) {
        zone_t *z = s_zone[i];
        if (z == NULL) {
            continue;
        }
        z->saw_enter = false;
        z->saw_leave = false;
        z->seen_frame = false;
        z->purpose_set = false;
        z->cur_n = 0;
    }
    open_zones(snap);
}

/** 收口当前秒。环境从未有效则清空并返回 false。 */
static bool commit_open(void)
{
    float row[COLS];
    if (!env_row(&s_open_snap, row)) {
        wipe();
        return false;
    }
    char room[PEER_STR16 + 1];
    field_str(room, s_open_snap.room_type);
    s_room = map_exact(room, k_room, 7, 6);
    s_committed = s_open_snap;
    slide_in(s_env, s_row_utc, &s_env_filled, row, s_open_utc, true);

    for (int i = 0; i < ZONE_MAX; i++) {
        zone_t *z = s_zone[i];
        if (z == NULL) {
            continue;
        }
        float radar[COLS];
        memcpy(radar, z->counts, 4 * sizeof(float));
        /* 又进又出记离开。只有进入记 1，都没有记 0。 */
        if (z->saw_leave) {
            radar[4] = 2.0f;
        } else if (z->saw_enter) {
            radar[4] = 1.0f;
        } else {
            radar[4] = 0.0f;
        }
        slide_in(z->radar, NULL, &z->filled, radar, 0, false);
        memcpy(z->prev_ids, z->cur_ids, z->cur_n);
        z->prev_n = z->cur_n;
        z->out_n = z->stage_n;
        memcpy(z->out_targets, z->stage, z->stage_n * sizeof(z->stage[0]));
        if (z->purpose_set) {
            memcpy(z->purpose, z->purpose_now, PEER_STR16);
            z->purpose[PEER_STR16 - 1] = '\0';
            z->model_zone = map_exact(z->purpose, k_purpose, 7, 5);
        }
    }

    if (s_env_filled == ROWS) {
        if (!s_fired_first) {
            s_beat = true;
            s_fired_first = true;
            s_since = 0;
        } else {
            s_since++;
            if (s_since >= 2) {
                s_beat = true;
                s_since = 0;
            }
        }
    }
    return true;
}

int peer_model_top_class(const float prob[3])
{
    int best = 0;
    if (prob == NULL) {
        return 0;
    }
    for (int i = 1; i < 3; i++) {
        if (prob[i] > prob[best]) {
            best = i;
        }
    }
    return best;
}

bool peer_model_report_ok(const float prob[3], int class_index)
{
    if (prob == NULL || class_index < 0 || class_index > 2) {
        return false;
    }
    return prob[class_index] > 0.6f;
}

void peer_model_window_reset(void)
{
    for (int i = 0; i < ZONE_MAX; i++) {
        zone_free(s_zone[i]);
        s_zone[i] = NULL;
    }
    s_fail_n = 0;
    memset(s_hold_ok, 0, sizeof(s_hold_ok));
    memset(s_hold, 0, sizeof(s_hold));
    s_room = 6;
    s_env_filled = 0;
    s_fired_first = false;
    s_since = 0;
    s_beat = false;
    s_have_open = false;
    memset(&s_committed, 0, sizeof(s_committed));
}

void peer_model_window_invalidate(void)
{
    wipe();
}

bool peer_model_window_on_snapshot(const peer_snapshot_t *snap)
{
    if (snap == NULL) {
        return false;
    }
    if (!s_have_open) {
        begin_open(snap);
        return false;
    }
    if (snap->utc_unix_s == s_open_utc) {
        s_open_snap = *snap;
        open_zones(snap);
        return false;
    }
    /* 秒号跳过 1 秒以上，或时钟回退，都不能接着用旧窗口。 */
    if (snap->utc_unix_s != s_open_utc + 1) {
        wipe();
        begin_open(snap);
        return false;
    }
    bool beat = false;
    if (commit_open()) {
        beat = s_beat;
    }
    begin_open(snap);
    return beat;
}

int peer_model_window_zones(peer_model_zone_in_t *out, int max)
{
    s_beat = false;
    if (out == NULL || max <= 0 || s_env_filled < ROWS) {
        return 0;
    }
    int ids[ZONE_MAX];
    int n = 0;
    for (int i = 0; i < ZONE_MAX; i++) {
        if (s_zone[i] != NULL && s_zone[i]->filled >= ROWS) {
            ids[n++] = i;
        }
    }
    for (int a = 1; a < n; a++) {
        int key = ids[a];
        int b = a;
        while (b > 0 && s_zone[ids[b - 1]]->zone_id > s_zone[key]->zone_id) {
            ids[b] = ids[b - 1];
            b--;
        }
        ids[b] = key;
    }
    int w = n < max ? n : max;
    for (int i = 0; i < w; i++) {
        zone_t *z = s_zone[ids[i]];
        peer_model_zone_in_t *d = &out[i];
        memset(d, 0, sizeof(*d));
        d->zone_id = z->zone_id;
        d->model_zone = z->model_zone;
        d->room = s_room;
        d->epoch_sec = (double)s_row_utc[0];
        memcpy(d->radar, z->radar, sizeof(d->radar));
        memcpy(d->env, s_env, sizeof(d->env));
        memcpy(d->purpose, z->purpose, PEER_STR16);
    }
    return w;
}

bool peer_model_window_info(int32_t zone_id, peer_model_zone_info_t *out)
{
    zone_t *z = find_zone(zone_id);
    if (z == NULL || out == NULL) {
        return false;
    }
    out->filled = z->filled;
    out->model_zone = z->model_zone;
    out->last_class = z->last_class;
    memcpy(out->radar, z->radar, sizeof(out->radar));
    return true;
}

bool peer_model_window_event(int32_t zone_id, int class_index, peer_event_t *ev)
{
    static const peer_evt_id_t k_class_evt[3] = {
        PEER_EVT_ID_RESTING_BED,
        PEER_EVT_ID_SLEEP_IN_NIGHT_BED,
        PEER_EVT_ID_WORK_OFFICE,
    };
    zone_t *z = find_zone(zone_id);
    if (z == NULL || ev == NULL || class_index < 0 || class_index > 2) {
        return false;
    }
    /* 类名没变就不填事件，调用方也不该入队。 */
    if (z->last_class == class_index) {
        return false;
    }
    const char *name = peer_evt_wire_name(k_class_evt[class_index]);
    if (name == NULL) {
        return false;
    }
    memset(ev, 0, sizeof(*ev));
    strncpy(ev->event_type, name, sizeof(ev->event_type) - 1);
    ev->zone_id = z->zone_id;
    memcpy(ev->purpose, z->purpose, PEER_STR16);
    ev->purpose[PEER_STR16 - 1] = '\0';
    ev->people_room = s_committed.people_room;
    ev->co2_ppm = s_committed.co2_ppm;
    ev->temp_c_x10 = s_committed.temp_c_x10;
    ev->rh_x10 = s_committed.rh_x10;
    ev->lux = s_committed.lux;
    ev->sound_db_x10 = s_committed.sound_db_x10;
    memcpy(ev->room_type, s_committed.room_type, PEER_STR16);
    ev->room_type[PEER_STR16 - 1] = '\0';
    ev->target_n = z->out_n;
    memcpy(ev->targets, z->out_targets, z->out_n * sizeof(z->out_targets[0]));
    return true;
}

void peer_model_window_event_sent(int32_t zone_id, int class_index)
{
    zone_t *z = find_zone(zone_id);
    if (z == NULL) {
        return;
    }
    z->last_class = class_index;
}
