# 相同事件 30 秒冷却 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 相同 `event_type`、`zone_id`、`env_ch` 在 30 秒内只入队第一条，满 30 秒后的下一条照常入队。

**Architecture:** 冷却表放在 `peer_judge` 已有的队列锁里。内部边沿入队和 `peer_judge_push` 都走 `enqueue_unlocked`。成功写入才记时间。冷却丢掉时 `peer_judge_push` 返回 true，队列满返回 false。自检用文件内毫秒覆盖，不新增测试文件。

**Tech Stack:** 现有 C11 `peer_judge`、ESP-IDF 上的 `esp_timer_get_time()`、PC 上的 `gcc -std=c11` 自检。

## Global Constraints

- 规格：`docs/superpowers/specs/2026-10-29-same-event-cooldown-design.md`
- 键：`event_type`、`env_ch` 按 C 字符串比较，不超过字段长度；没有结尾 0 时比到字段容量。`zone_id` 整数相等。空字符串参与比较。
- 窗口 30000 毫秒。小于 30000 丢掉且不改上次时间。大于或等于 30000，或表里没有这把键，则入队并记下现在。
- `peer_judge_push`：空指针返回 false。冷却丢掉返回 true 且不入队。队列已有 24 条返回 false，不记冷却，队头不变。
- `peer_judge_reset` 在同一把锁里清掉冷却表。
- 设备时钟是 `esp_timer_get_time()` 的毫秒。不用 `utc_unix_s`。
- 自检可把「现在」设成指定毫秒。无论成败，这段自检返回前关掉覆盖。PC 上只使用该毫秒计数。
- 冷却表 32 槽。都占用时覆盖 `last_ms` 最小的槽；时间相同则覆盖先找到的槽。
- 不新增测试文件，不改 `peer_event_t` 布局，不改边沿规则和模型「类名不变不填事件」。

## File Structure

- `components/peer_event/peer_judge.c`：冷却表、入队判断、自检用例。边沿和模型共用这里。
- `components/peer_event/include/peer_judge.h`：只改 `peer_judge_push` 的注释。
- `README.md`：在队列说明后加一句 30 秒规则。
- `components/peer_event/test_peer_judge.c`：不改。仍然只调用 `peer_judge_selfcheck`。

---

### Task 1: 入队前按键冷却 30 秒

**Files:**
- Modify: `components/peer_event/peer_judge.c`
- Modify: `components/peer_event/include/peer_judge.h`
- Modify: `README.md`
- Test: `components/peer_event/test_peer_judge.c`（已有入口，本任务不改它）

**Interfaces:**
- Consumes: `peer_judge_push(const peer_event_t *ev)`、`peer_judge_reset(void)`、`peer_judge_queue_len(void)`、`peer_judge_peek(peer_event_t *out)`、`q_lock` / `q_unlock`、`s_q` / `s_q_n` / `Q_MAX`
- Produces: 签名不变。`peer_judge_push` 在冷却丢掉时改为返回 true。新增的 `enqueue_unlocked`、`cool_cases`、`test_now`、`test_now_off` 都是本文件 static，外部看不到。

- [x] **Step 1: 写会失败的自检**

在 `components/peer_event/peer_judge.c` 的队列静态变量 `s_q_n` 之后加上时钟覆盖。此时还不要拦截入队。

```c
#define COOL_MS 30000

static int64_t s_now_ms;
static bool s_now_fixed;

static void test_now(int64_t ms)
{
    s_now_ms = ms;
    s_now_fixed = true;
}

static void test_now_off(void)
{
    s_now_fixed = false;
}
```

在 `peer_judge_selfcheck` 前面加上静态自检。`peer_event_t`、`memset`、`memcpy`、`strcmp` 已由现有包含可用。

```c
/** 冷却和「24 个不同区撑满队列」。失败时调用方负责关掉测试时钟。 */
static bool cool_cases(void)
{
    peer_event_t ev;
    peer_judge_reset();
    test_now(0);
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.event_type, "resting", 8);
    ev.zone_id = 3;
    if (!peer_judge_push(&ev) || peer_judge_queue_len() != 1) {
        return false;
    }
    /* 中间这次被丢掉，也不能把 30 秒窗口从 0 ms 往后推。 */
    test_now(10000);
    if (!peer_judge_push(&ev) || peer_judge_queue_len() != 1) {
        return false;
    }
    test_now(29999);
    if (!peer_judge_push(&ev) || peer_judge_queue_len() != 1) {
        return false;
    }
    test_now(30000);
    if (!peer_judge_push(&ev) || peer_judge_queue_len() != 2) {
        return false;
    }
    ev.zone_id = 4;
    if (!peer_judge_push(&ev) || peer_judge_queue_len() != 3) {
        return false;
    }
    ev.zone_id = 3;
    memcpy(ev.env_ch, "co2", 4);
    if (!peer_judge_push(&ev) || peer_judge_queue_len() != 4) {
        return false;
    }
    peer_judge_reset();
    test_now(30000);
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.event_type, "resting", 8);
    ev.zone_id = 3;
    if (!peer_judge_push(&ev) || peer_judge_queue_len() != 1) {
        return false;
    }

    peer_judge_reset();
    for (int i = 0; i < 24; i++) {
        memset(&ev, 0, sizeof(ev));
        memcpy(ev.event_type, "resting", 8);
        ev.zone_id = i;
        if (!peer_judge_push(&ev)) {
            return false;
        }
    }
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.event_type, "resting", 8);
    ev.zone_id = 24;
    peer_event_t head;
    if (peer_judge_push(&ev) || peer_judge_queue_len() != 24 || !peer_judge_peek(&head) ||
        head.zone_id != 0 || strcmp(head.event_type, "resting") != 0) {
        return false;
    }
    peer_judge_reset();
    return true;
}
```

删掉 `peer_judge_selfcheck` 末尾从「模型任务和边沿共用 24 条队列」到函数结束的旧 24 次相同 `resting` 入队。换成：

```c
    if (!cool_cases()) {
        test_now_off();
        return false;
    }
    test_now_off();
    return true;
```

`peer_judge_selfcheck` 前面原有的 `peer_judge_reset()` 和映射表名字检查保留。

- [x] **Step 2: 跑自检，确认红灯是因为还没过滤**

在仓库根目录执行：

```text
gcc -std=c11 -I components/peer_frame/include -I components/peer_event/include components/peer_frame/peer_frame.c components/peer_event/peer_event_map.c components/peer_event/peer_judge.c components/peer_event/test_peer_judge.c -o test_peer_judge.exe
./test_peer_judge.exe
```

Expected: 进程退出码 1，标准错误是 `peer_judge_selfcheck failed`。10000 ms 那一次仍会入队，队列长度变成 2，`cool_cases` 返回 false。编译失败要先修编译错误，再看这次断言。

- [x] **Step 3: 写最小实现**

ESP 平台那段包含改为：

```c
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#endif
```

把 Step 1 的时钟静态变量换成下面这一段（含 `now_ms`）：

```c
#define COOL_MAX 32
#define COOL_MS  30000

typedef struct {
    char event_type[PEER_EVENT_STR_MAX];
    char env_ch[PEER_STR16];
    int32_t zone_id;
    int64_t last_ms;
    bool used;
} cool_t;

static cool_t s_cool[COOL_MAX];
static int64_t s_now_ms;
static bool s_now_fixed;

static int64_t now_ms(void)
{
#if defined(ESP_PLATFORM)
    if (!s_now_fixed) {
        return (int64_t)(esp_timer_get_time() / 1000);
    }
#endif
    return s_now_ms;
}

static void test_now(int64_t ms)
{
    s_now_ms = ms;
    s_now_fixed = true;
}

static void test_now_off(void)
{
    s_now_fixed = false;
}

/** 比到结尾 0 或字段容量。容量内没有 0 则整段都要相等。 */
static bool field_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
        if (a[i] == '\0') {
            return true;
        }
    }
    return true;
}

static bool cool_same(const cool_t *c, const peer_event_t *ev)
{
    return c->used && c->zone_id == ev->zone_id &&
           field_eq(c->event_type, ev->event_type, sizeof(c->event_type)) &&
           field_eq(c->env_ch, ev->env_ch, sizeof(c->env_ch));
}

static int cool_find(const peer_event_t *ev)
{
    for (int i = 0; i < COOL_MAX; i++) {
        if (cool_same(&s_cool[i], ev)) {
            return i;
        }
    }
    return -1;
}

static bool cool_blocked(const peer_event_t *ev)
{
    int i = cool_find(ev);
    if (i < 0) {
        return false;
    }
    return now_ms() - s_cool[i].last_ms < (int64_t)COOL_MS;
}

static void cool_write(int i, const peer_event_t *ev)
{
    cool_t *c = &s_cool[i];
    memset(c, 0, sizeof(*c));
    memcpy(c->event_type, ev->event_type, sizeof(c->event_type));
    memcpy(c->env_ch, ev->env_ch, sizeof(c->env_ch));
    c->zone_id = ev->zone_id;
    c->last_ms = now_ms();
    c->used = true;
}

static void cool_stamp(const peer_event_t *ev)
{
    int i = cool_find(ev);
    if (i < 0) {
        for (int k = 0; k < COOL_MAX; k++) {
            if (!s_cool[k].used) {
                i = k;
                break;
            }
        }
    }
    if (i < 0) {
        i = 0;
        for (int k = 1; k < COOL_MAX; k++) {
            if (s_cool[k].last_ms < s_cool[i].last_ms) {
                i = k;
            }
        }
    }
    cool_write(i, ev);
}
```

用下面的函数替换 `push_unlocked`、`push` 和 `peer_judge_push`。边沿的 `push` 继续不看返回值。

```c
/**
 * 调用方已持有队列锁。
 * 冷却期内丢掉并返回 true。队列满返回 false，不记冷却。
 * 成功写入后才盖时间戳。
 */
static bool enqueue_unlocked(const peer_event_t *ev)
{
    if (cool_blocked(ev)) {
        return true;
    }
    if (s_q_n >= Q_MAX) {
        return false; /* ponytail: 队列满则丢最新。模型一拍最多 16 条，24 仍可能被边沿挤满。 */
    }
    s_q[s_q_n++] = *ev;
    cool_stamp(ev);
    return true;
}

/** 边沿判定内部入队。冷却丢掉或队列满都不入队。 */
static void push(const peer_event_t *ev)
{
    q_lock();
    (void)enqueue_unlocked(ev);
    q_unlock();
}

bool peer_judge_push(const peer_event_t *ev)
{
    if (ev == NULL) {
        return false;
    }
    q_lock();
    bool ok = enqueue_unlocked(ev);
    q_unlock();
    return ok;
}
```

`peer_judge_reset` 在 `s_q_n = 0;` 之后、解锁之前加上：

```c
    memset(s_cool, 0, sizeof(s_cool));
```

- [x] **Step 4: 再跑自检，确认通过**

```text
gcc -std=c11 -I components/peer_frame/include -I components/peer_event/include components/peer_frame/peer_frame.c components/peer_event/peer_event_map.c components/peer_event/peer_judge.c components/peer_event/test_peer_judge.c -o test_peer_judge.exe
./test_peer_judge.exe
```

Expected: 打印 `ok`，退出码 0。

- [x] **Step 5: 改对外注释和 README**

`components/peer_event/include/peer_judge.h` 里 `peer_judge_push` 的注释换成：

```c
/**
 * 模型任务入队一条已经填好的事件。
 * 相同 event_type、zone_id、env_ch 距上次成功入队不满 30 秒：丢掉并返回 true。
 * 队列已满返回 false，不记冷却时间，队头保持不变。ev 为空返回 false。
 * ev 只在本次调用内读取。
 */
bool peer_judge_push(const peer_event_t *ev);
```

`README.md` 里「队列最多 24 条」那一段落后加一句：

```text
相同 `event_type`、`zone_id`、`env_ch` 在 30 秒内只入队第一条，满 30 秒后的下一条照常入队。
```

- [x] **Step 6: 注释改完后再跑一次自检**

命令与 Step 4 相同。Expected: 仍然打印 `ok`，退出码 0。

- [ ] **Step 7: 提交**

```text
git add components/peer_event/peer_judge.c components/peer_event/include/peer_judge.h README.md docs/superpowers/specs/2026-10-29-same-event-cooldown-design.md docs/superpowers/plans/2026-10-29-same-event-cooldown.md
git commit -m "filter duplicate peer events for 30 seconds"
```

不提交 `test_peer_judge.exe` 和 `build/`。
