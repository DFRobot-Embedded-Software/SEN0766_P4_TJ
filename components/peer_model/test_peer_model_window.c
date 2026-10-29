#include "peer_model_window.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "fail: %s\n", msg);
        g_fail = 1;
    }
}

static peer_snapshot_t snap_at(uint32_t sec)
{
    peer_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.utc_unix_s = sec;
    s.radar_ready = 1;
    s.co2_ppm = 500;
    s.temp_c_x10 = 260;
    s.rh_x10 = 500;
    s.lux = 100;
    s.sound_db_x10 = 400;
    memcpy(s.room_type, "bedroom", 8);
    return s;
}

static void add_person(peer_snapshot_t *s, uint8_t id, uint8_t kin, int32_t zone, const char *purpose)
{
    peer_target_t *t = &s->targets[s->target_n++];
    t->track_id = id;
    t->kinesia = kin;
    t->zone_id = zone;
    memcpy(t->purpose, purpose, strlen(purpose));
    s->people_room = s->target_n;
}

static bool feed(uint32_t sec, int32_t zone, const char *purpose)
{
    peer_snapshot_t s = snap_at(sec);
    if (zone != 0) {
        add_person(&s, 1, 1, zone, purpose);
    }
    return peer_model_window_on_snapshot(&s);
}

int main(void)
{
    float tie[3] = {0.1f, 0.4f, 0.4f};
    expect(peer_model_top_class(tie) == 1, "tie keeps earlier class");
    float low[3] = {0.6f, 0.2f, 0.2f};
    float high[3] = {0.61f, 0.2f, 0.19f};
    expect(!peer_model_report_ok(low, 0), "weight 0.6 does not report");
    expect(peer_model_report_ok(high, 0), "weight above 0.6 reports");
    expect(!peer_model_report_ok(high, 3), "class out of range");
    expect(!peer_model_report_ok(NULL, 0), "null prob");

    peer_model_window_reset();
    peer_snapshot_t a = snap_at(100);
    add_person(&a, 1, 1, 3, "bed1");
    expect(!peer_model_window_on_snapshot(&a), "open second is not a beat");
    a = snap_at(100);
    add_person(&a, 1, 90, 3, "bed1");
    expect(!peer_model_window_on_snapshot(&a), "same second still open");
    expect(!feed(101, 3, "bed1"), "first commit is not a beat");
    peer_model_zone_info_t info;
    expect(peer_model_window_info(3, &info), "zone 3 exists");
    expect(info.filled == 1, "one row");
    expect(info.model_zone == 0, "bed1");
    expect(info.radar[0] == 0 && info.radar[1] == 0 && info.radar[2] == 0 && info.radar[3] == 1.0f &&
               info.radar[4] == 1.0f,
           "last frame fast, enter");

    peer_model_window_reset();
    expect(!feed(100, 3, "bed1"), "seed");
    a = snap_at(101);
    expect(!peer_model_window_on_snapshot(&a), "leave frame");
    a = snap_at(101);
    add_person(&a, 2, 1, 3, "bed1");
    expect(!peer_model_window_on_snapshot(&a), "enter later same second");
    expect(!feed(102, 3, "bed1"), "commit mixed second");
    expect(peer_model_window_info(3, &info), "zone still there");
    expect(info.radar[5] == 1.0f && info.radar[9] == 2.0f, "both ways count as leave");

    peer_model_window_reset();
    expect(!feed(100, 4, "bed"), "unknown purpose");
    expect(!feed(101, 4, "bed"), "commit bed");
    expect(peer_model_window_info(4, &info) && info.model_zone == 5, "bed is generic");

    peer_model_window_reset();
    a = snap_at(100);
    a.co2_ppm = -1;
    add_person(&a, 1, 1, 3, "bed1");
    peer_model_window_on_snapshot(&a);
    a = snap_at(101);
    a.co2_ppm = -1;
    add_person(&a, 1, 1, 3, "bed1");
    peer_model_window_on_snapshot(&a);
    expect(peer_model_window_info(3, &info) && info.filled == 0, "never-valid co2 wipes");

    peer_model_window_reset();
    for (uint32_t sec = 100; sec <= 108; sec++) {
        expect(!feed(sec, 3, "bed1"), "fill hold");
    }
    a = snap_at(109);
    a.co2_ppm = -1;
    memset(a.room_type, 0, sizeof(a.room_type));
    memcpy(a.room_type, "living", 6);
    add_person(&a, 1, 1, 3, "bed1");
    expect(!peer_model_window_on_snapshot(&a), "sec 109 not yet closed");
    a = snap_at(110);
    memset(a.room_type, 0, sizeof(a.room_type));
    memcpy(a.room_type, "living", 6);
    add_person(&a, 1, 1, 3, "bed1");
    expect(peer_model_window_on_snapshot(&a), "tenth commit beats");
    peer_model_zone_in_t zones[4];
    int n = peer_model_window_zones(zones, 4);
    expect(n == 1 && zones[0].env[9 * 5 + 2] == 500.0f, "held co2");
    expect(zones[0].room == 1, "living");
    expect(zones[0].epoch_sec == 100.0, "oldest second");

    peer_model_window_reset();
    expect(!feed(100, 3, "bed1"), "open 100");
    expect(!feed(102, 3, "bed1"), "jump clears");
    expect(peer_model_window_info(3, &info) && info.filled == 0, "jump filled 0");

    peer_model_window_reset();
    /* 一帧最多 8 个目标。16 个区分成同一秒的两帧，槽位会留下来。 */
    a = snap_at(200);
    for (int z = 1; z <= 8; z++) {
        add_person(&a, (uint8_t)z, 1, z, "sofa");
    }
    peer_model_window_on_snapshot(&a);
    a = snap_at(200);
    for (int z = 9; z <= 16; z++) {
        add_person(&a, (uint8_t)z, 1, z, "sofa");
    }
    peer_model_window_on_snapshot(&a);
    expect(peer_model_window_info(1, &info) && peer_model_window_info(16, &info), "16 slots");
    a = snap_at(200);
    add_person(&a, 1, 1, 17, "sofa");
    peer_model_window_on_snapshot(&a);
    expect(!peer_model_window_info(17, &info), "17th ignored");

    peer_model_window_reset();
    for (uint32_t sec = 100; sec <= 109; sec++) {
        expect(!feed(sec, 3, "bed1"), "before first beat");
    }
    expect(feed(110, 3, "bed1"), "beat at 110");
    n = peer_model_window_zones(zones, 4);
    expect(n == 1 && zones[0].zone_id == 3 && zones[0].epoch_sec == 100.0, "window 100-109");
    expect(!feed(111, 3, "bed1"), "one more second waits");
    expect(feed(112, 3, "bed1"), "two seconds beat again");

    peer_model_window_reset();
    bool saw110 = false;
    for (uint32_t sec = 100; sec <= 110; sec++) {
        peer_snapshot_t s = snap_at(sec);
        add_person(&s, 1, 1, 3, "bed1");
        if (sec >= 105) {
            add_person(&s, 2, 1, 9, "desk");
        }
        bool beat = peer_model_window_on_snapshot(&s);
        if (sec == 110) {
            saw110 = beat;
            n = peer_model_window_zones(zones, 4);
            expect(n == 1 && zones[0].zone_id == 3, "new zone not ready");
        }
    }
    expect(saw110, "first beat still happens");
    bool saw116 = false;
    for (uint32_t sec = 111; sec <= 116; sec++) {
        peer_snapshot_t s = snap_at(sec);
        add_person(&s, 1, 1, 3, "bed1");
        add_person(&s, 2, 1, 9, "desk");
        bool beat = peer_model_window_on_snapshot(&s);
        if (sec == 116) {
            saw116 = beat;
            n = peer_model_window_zones(zones, 4);
            expect(n == 2 && zones[0].zone_id == 3 && zones[1].zone_id == 9, "zone 9 joins");
            expect(zones[1].model_zone == 3, "desk");
        }
    }
    expect(saw116, "16th commit beats");

    peer_model_window_reset();
    for (uint32_t sec = 100; sec <= 109; sec++) {
        feed(sec, 3, "bed1");
    }
    expect(feed(110, 3, "bed1"), "event beat");
    peer_model_window_zones(zones, 4);
    peer_event_t ev;
    expect(peer_model_window_event(3, 0, &ev), "first class reports");
    expect(strcmp(ev.event_type, "resting_bed") == 0 && ev.env_ch[0] == '\0' && ev.zone_id == 3 &&
               ev.target_n == 1 && strcmp(ev.targets[0].motion, "still") == 0,
           "event fields");
    peer_model_window_event_sent(3, 0);
    expect(!peer_model_window_event(3, 0, &ev), "same class stays quiet");
    expect(!peer_model_window_event(3, 3, &ev), "old 6-class index rejected");
    expect(peer_model_window_event(3, 1, &ev), "new class reports");
    expect(strcmp(ev.event_type, "sleep_in_night_bed") == 0, "class 1 name");
    expect(peer_model_window_event(3, 2, &ev), "work class reports");
    expect(strcmp(ev.event_type, "work_office") == 0, "class 2 name");
    expect(peer_model_window_event(3, 1, &ev), "unsent class reports again");

    if (g_fail) {
        return 1;
    }
    printf("ok\n");
    return 0;
}
