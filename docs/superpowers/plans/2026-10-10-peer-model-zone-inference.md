# 分区融合推理 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `board_1009` 的融合模型接进 P4 从机，按 `zone_id` 各维护 10 秒雷达窗口，全屋共用环境和 MIC1，只在各区第一名类名变化时经现有 `0x20` 上报。

**Architecture:** 新建 `peer_model`。纯 C 的窗口模块在 PC 上用假快照测试。串口回调只入队快照。麦任务写 16 秒左声道环。模型任务收秒、拷声音、跑 HAMIC 和 TFLite，再用 `peer_judge_push` 入队。`peer_judge` 的 PC 自检不链接 TFLite。

**Tech Stack:** ESP-IDF 5.5.5、ESP32-P4、TFLite Micro 1.4.0、ESP-DSP 1.8.2、ESP-NN 1.3.2、现有 `peer_frame` / `peer_event` / `es7210_mic`。

## Global Constraints

- 规格：`docs/superpowers/specs/2026-10-10-peer-model-zone-inference-design.md`
- 不改 `peer_snapshot_t`、`peer_env_cfg_t`、`peer_event_t` 的长度（393 / 26 / 136）。
- 事件编号 1–6 不改、不复用。模型类名追加为 7–12。
- 事件队列容量 24，满了丢最新的一条，不挤队头。
- 最多 16 个 `zone_id`。第 17 个和申请失败的区忽略。槽位不释放。
- 新写的头文件、对外函数、结构体字段，以及收口、清空、跳过、第一名没变、入队失败这些分支，用中文注释，写法对齐 `peer_frame.h`。
- 从 `board_1009` 原样复制的权重和算子保留原文注释。
- 模型任务不写 UART。`0x11` 不进模型。
- PC 自检命令不链接 ESP-IDF、TFLite、麦克风。

## File Structure

- `components/peer_event/include/peer_event_map.h`、`peer_event_map.c`：追加 6 个类名。
- `components/peer_event/include/peer_judge.h`、`peer_judge.c`：`peer_judge_push`、队列 24、设备上的互斥。PC 上锁为空。
- `components/peer_model/include/peer_model_window.h`、`peer_model_window.c`：秒窗口。只依赖 `peer_frame.h` 和 `peer_event_map`。
- `components/peer_model/test_peer_model_window.c`：PC 自检。
- `components/peer_model/vendor/`：从 demo 复制的 `fusion_native`、`fusion_model.h`、HAMIC、`esp_dsp_fft`。不复制 WAV 读取和 `main.cc`。
- `components/peer_model/peer_model.cc`、`include/peer_model.h`：快照队列、麦环、推理任务。
- `main/SEN0766_P4_TJ.c`：快照再交一份给模型；删除原来的麦队列任务。
- `README.md`：队列长度和 6 个新事件名。

---

### Task 1: 映射表加上 6 个类名

**Files:**
- Modify: `components/peer_event/include/peer_event_map.h`
- Modify: `components/peer_event/peer_event_map.c`
- Modify: `components/peer_event/peer_judge.c`（只在 `peer_judge_selfcheck` 末尾加断言）

**Interfaces:**
- Consumes: 现有 `peer_evt_wire_name`
- Produces: `PEER_EVT_ID_RESTING` = 7，随后依次 `SLEEP`、`RESTING_BED`、`SLEEP_IN_NIGHT_BED`、`WAKE_UP_IN_NIGHT_BED`、`EMPTY_BED` = 12。英文名与规格一致。

- [ ] **Step 1: 在自检末尾写上会失败的断言**

在 `peer_judge_selfcheck` 的 `return true` 之前插入：

```c
    if (strcmp(peer_evt_wire_name(PEER_EVT_ID_RESTING), "resting") != 0 ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_SLEEP), "sleep") != 0 ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_RESTING_BED), "resting_bed") != 0 ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_SLEEP_IN_NIGHT_BED), "sleep_in_night_bed") != 0 ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_WAKE_UP_IN_NIGHT_BED), "wake_up_in_night_bed") != 0 ||
        strcmp(peer_evt_wire_name(PEER_EVT_ID_EMPTY_BED), "empty_bed") != 0) {
        return false;
    }
```

`peer_judge.c` 已包含 `<string.h>`。

- [ ] **Step 2: 编译并确认失败**

```text
gcc -std=c11 -Wall -Wextra -I components/peer_frame/include -I components/peer_event/include components/peer_frame/peer_frame.c components/peer_event/peer_event_map.c components/peer_event/peer_judge.c components/peer_event/test_peer_judge.c -o test_peer_judge.exe
```

Expected: 编译失败，`PEER_EVT_ID_RESTING` 未定义。

- [ ] **Step 3: 追加枚举和表项**

`peer_event_map.h` 的枚举在 `PEER_EVT_ID_ENV_RECOVERED = 6` 后追加，文件头注释里的编号列表同步加上 7–12：

```c
    PEER_EVT_ID_RESTING = 7,
    PEER_EVT_ID_SLEEP = 8,
    PEER_EVT_ID_RESTING_BED = 9,
    PEER_EVT_ID_SLEEP_IN_NIGHT_BED = 10,
    PEER_EVT_ID_WAKE_UP_IN_NIGHT_BED = 11,
    PEER_EVT_ID_EMPTY_BED = 12
```

`peer_event_map.c` 的数组末尾追加。注释写明这 6 个是模型第一名，只在类名变化时由 `peer_model` 入队：

```c
    {PEER_EVT_ID_RESTING, "resting"},
    {PEER_EVT_ID_SLEEP, "sleep"},
    {PEER_EVT_ID_RESTING_BED, "resting_bed"},
    {PEER_EVT_ID_SLEEP_IN_NIGHT_BED, "sleep_in_night_bed"},
    {PEER_EVT_ID_WAKE_UP_IN_NIGHT_BED, "wake_up_in_night_bed"},
    {PEER_EVT_ID_EMPTY_BED, "empty_bed"},
```

- [ ] **Step 4: 再跑自检**

同一条 `gcc` 命令，然后 `test_peer_judge.exe`。Expected: 打印 `ok`。

- [ ] **Step 5: Commit**

```text
git add components/peer_event/include/peer_event_map.h components/peer_event/peer_event_map.c components/peer_event/peer_judge.c
git commit -m "feat: add fusion class names to the event map"
```

---

### Task 2: 事件队列扩到 24，并给模型任务一个入队口

**Files:**
- Modify: `components/peer_event/include/peer_judge.h`
- Modify: `components/peer_event/peer_judge.c`
- Modify: `components/peer_event/CMakeLists.txt`（`REQUIRES` 增加 `freertos`）

**Interfaces:**
- Consumes: `peer_event_t`
- Produces: `bool peer_judge_push(const peer_event_t *ev)`。成功入队返回 true。满了返回 false，队列内容不变。

PC 上没有 FreeRTOS。锁只在 `ESP_PLATFORM` 下创建。PC 自检是单线程，空锁即可。

- [ ] **Step 1: 在自检末尾写队列测试**

放在 Task 1 的类名断言之后、`return true` 之前：

```c
    peer_judge_reset();
    peer_event_t pushed;
    memset(&pushed, 0, sizeof(pushed));
    memcpy(pushed.event_type, "resting", 8);
    for (int i = 0; i < 24; i++) {
        if (!peer_judge_push(&pushed)) {
            return false;
        }
    }
    if (peer_judge_push(&pushed) || peer_judge_queue_len() != 24) {
        return false;
    }
    peer_event_t head;
    if (!peer_judge_peek(&head) || strcmp(head.event_type, "resting") != 0) {
        return false;
    }
    peer_judge_reset();
```

- [ ] **Step 2: 跑自检，确认链接失败**

Expected: `peer_judge_push` 未定义。

- [ ] **Step 3: 实现锁、容量和 push**

`Q_MAX` 改为 24。注释改为：边沿和模型事件共用。满了丢最新的一条，避免挤掉更早的进出房。

在文件顶部静态区附近增加：

```c
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_q_mu;

/** 设备上保护队列。首次上锁时创建。PC 自检不编译这段。 */
static void q_lock(void)
{
    if (s_q_mu == NULL) {
        s_q_mu = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_q_mu, portMAX_DELAY);
}

static void q_unlock(void)
{
    xSemaphoreGive(s_q_mu);
}
#else
static void q_lock(void) {}
static void q_unlock(void) {}
#endif
```

把现有 `push` 改成已持锁的 `push_unlocked`。对外和内部入队都先上锁：

```c
/** 调用方已持有队列锁。满了直接返回。 */
static void push_unlocked(const peer_event_t *ev)
{
    if (s_q_n >= Q_MAX) {
        return;
    }
    s_q[s_q_n++] = *ev;
}

/** 边沿判定内部入队。满了丢这一条。 */
static void push(const peer_event_t *ev)
{
    q_lock();
    push_unlocked(ev);
    q_unlock();
}

bool peer_judge_push(const peer_event_t *ev)
{
    if (ev == NULL) {
        return false;
    }
    q_lock();
    bool ok = s_q_n < Q_MAX;
    if (ok) {
        push_unlocked(ev);
    }
    q_unlock();
    return ok;
}
```

`peer_judge_peek`、`peer_judge_pop`、`peer_judge_queue_len`、`peer_judge_reset` 整段函数体用 `q_lock` / `q_unlock` 包住。`reset` 里不要再调用会自行上锁的函数。

`peer_judge.h` 在 `peer_judge_queue_len` 前声明，并改文件头第一句：边沿判定给 `peer_event` 用；模型任务只通过 `peer_judge_push` 把一条 `0x20` 放进同一队列。

```c
/**
 * 模型任务入队一条已经填好的事件。
 * 满了返回 false，队头保持不变。ev 只在本次调用内读取。
 */
bool peer_judge_push(const peer_event_t *ev);
```

`CMakeLists.txt` 的 `REQUIRES` 加上 `freertos`。

- [ ] **Step 4: 跑自检**

Expected: `ok`。

- [ ] **Step 5: Commit**

```text
git add components/peer_event/include/peer_judge.h components/peer_event/peer_judge.c components/peer_event/CMakeLists.txt
git commit -m "feat: let the model push events onto the shared queue"
```

---

### Task 3: 秒窗口

**Files:**
- Create: `components/peer_model/include/peer_model_window.h`
- Create: `components/peer_model/peer_model_window.c`
- Create: `components/peer_model/test_peer_model_window.c`
- Create: `components/peer_model/CMakeLists.txt`（本任务只编窗口；Task 4 再把 vendor 加进去）

**Interfaces:**
- Consumes: `peer_snapshot_t`、`peer_event_t`、`peer_evt_wire_name`、`PEER_EVT_ID_RESTING` 起的连续编号
- Produces:
  - `int peer_model_top_class(const float prob[6])`
  - `void peer_model_window_reset(void)`
  - `void peer_model_window_invalidate(void)`
  - `bool peer_model_window_on_snapshot(const peer_snapshot_t *snap)` — 这一下刚好把一拍推理触发掉时返回 true
  - `int peer_model_window_zones(peer_model_zone_in_t *out, int max)` — 取走这一拍，清掉触发标志
  - `bool peer_model_window_info(int32_t zone_id, peer_model_zone_info_t *out)`
  - `bool peer_model_window_event(int32_t zone_id, int class_index, peer_event_t *ev)`
  - `void peer_model_window_event_sent(int32_t zone_id, int class_index)`

收秒规则写死在实现里，测试按这个数：

- 收到 `utc = T` 只是打开 T 这一秒。
- 收到 `utc = T+1` 才收口 T。连续收口满 10 秒的那一下，`on_snapshot` 返回 true。那 10 行是 T-9 … T。
- 之后每再成功收口 2 秒返回一次 true。
- `epoch_sec` 是这 10 行里最旧一行的 `utc_unix_s`。

- [ ] **Step 1: 写失败测试**

`test_peer_model_window.c` 的 `main` 依次调用下面这些检查，任一失败就 `fprintf` 到 stderr 并返回 1，全过打印 `ok`。

辅助函数：`snap_at(sec)` 把五路环境设成 CO₂ 500、温度 260、湿度 500、照度 100、声级 400，`room_type` 为 `"bedroom"`。`add_person` 往 `targets[]` 追加一条。

断言：

1. `peer_model_top_class` 对 `{0.1, 0.4, 0.4, 0, 0, 0}` 返回 1（相同概率保留更前面的类）。
2. 秒 100 两帧：第一帧 `kinesia=1`，第二帧 `kinesia=90`，都在 zone 3、purpose `"bed1"`。再用秒 101 收口。`info` 的 `radar[0]=0, [1]=0, [2]=0, [3]=1, [4]=1`（新区相对空轨迹集是进入），`model_zone==0`，`filled==1`。
3. 秒 100 有 track 1。秒 101 先一帧没人，再一帧 track 2、`kinesia=1`。秒 102 收口后，秒 101 那一行的事件码是 2，人数是 1 个静止。
4. purpose `"bed"` 的区 `model_zone==5`。`room_type` `"living"` 在凑满后的 `peer_model_zone_in_t.room==1`。
5. 环境列顺序是温度、湿度、CO₂、照度、声级，读 `zones()[i].env[t * 5 + c]`。第一秒 CO₂ 为 -1、其余有效，下一秒到来后所有区 `filled==0`。另起一组：秒 100 到 108 的 CO₂ 都是 500，秒 109 最后一帧 CO₂ 为 -1，喂秒 110 触发的 `zones()` 里 `env[9 * 5 + 2] == 500`。
6. 打开秒 100 后直接喂秒 102。`info.filled==0`。再从 102 连续走，不会把 100 算进行列。
7. 一帧里 16 个不同 `zone_id`（1…16）都申请成功。第 17 个 `info` 返回 false。
8. 从秒 100 起，zone 3 每秒都有一个人。`on_snapshot(110)` 返回 true，更早的秒返回 false。`zones()` 得到 1 个区，`epoch_sec==100`。紧接着的收口（喂 111）返回 false，喂 112 返回 true。
9. 秒 105 才出现的 zone 9，在第一次全屋推理（喂 110）时不在 `zones()` 结果里。它自己的 `filled` 要到后来才到 10。喂到 116 那一拍（第 16 次收口，见下文）它才出现。
10. 第一次 `zones()` 之后对 zone 3 取类 0：`event` 为 true，`event_type` 为 `resting`，`env_ch[0]==0`，`zone_id==3`。`event_sent` 之后再取类 0 为 false，取类 3 为 true。不调用 `event_sent` 时再取类 3 仍为 true。

第 9 条的节拍：第 1 次收口是秒 100（由喂 101 完成），第 10 次收口是秒 109（喂 110）并触发第一拍。之后第 12、14、16 次收口触发。第 16 次收口的是秒 115，由喂 116 完成。zone 9 从秒 105 开始有行，到秒 114 满 10 行，第 16 次收口时已经满，所以喂 116 的结果里有它。

- [ ] **Step 2: 编译测试，确认失败**

```text
gcc -std=c11 -Wall -Wextra -I components/peer_frame/include -I components/peer_event/include -I components/peer_model/include components/peer_event/peer_event_map.c components/peer_model/peer_model_window.c components/peer_model/test_peer_model_window.c -o test_peer_model_window.exe
```

Expected: 找不到 `peer_model_window.h` 或链接失败。

- [ ] **Step 3: 实现窗口**

头文件说明：这个模块把 `0x10` 收成每区 10 秒雷达和全屋 10 秒环境。不跑神经网络，不入队。`on_snapshot` 返回 true 表示调用方应立刻 `zones()`，用返回的输入做这一拍；推理期间不要再喂快照。

`peer_model_zone_in_t`：`zone_id`、`model_zone`、`room`、`epoch_sec`、`radar[50]`、`env[50]`、`purpose[16]`。

`peer_model_zone_info_t`：`filled`、`model_zone`、`last_class`（还没报过为 -1）、`radar[50]`。

`peer_model_window.c` 的行为：

- 槽位最多 16。每个槽第一次出现时 `calloc` 一块；`ESP_PLATFORM` 下改为 `heap_caps_calloc(1, sizeof(zone), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`。失败则这个 `zone_id` 以后都当没看见。
- 只要 `targets[]` 里出现 `zone_id != 0` 就占槽，哪怕 `kinesia` 不计数。
- 可计数：1 静止，2–30 微动，31–60 慢动，61–100 快动。0 和大于 100 不进入轨迹集合，也不进四类人数。
- 当前秒每一帧都用该区轨迹集合跟「上一帧」比。秒的第一帧，上一帧是上一秒收口时的集合；新区为空。新 id 记进入，消失记离开。四类人数和 purpose 只保留最后一帧。purpose 取该帧 `targets[]` 里这个区第一条可计数轨迹。这一帧没有可计数的人，purpose 保持上一秒。
- `purpose` / `room_type` 按 16 字节拷出并强制补 0 后再整段比较。对上规格里的 7 个小写英文才映射，否则 purpose 用 5、room 用 6。
- 环境列顺序是温度、湿度、CO₂、照度、声级。温度、湿度、声级把 ×10 除以 10。最后一帧某一路不是 -1，就更新这一路的「有效值」，即使这一秒因为别的路从未有效而整窗清空。五路都有有效值才收口；否则清空已提交行、当前秒、`filled`、上一拍类别和 2 秒计数，槽位和有效值保留。
- `utc` 比当前开着的秒大超过 1，或比它小，同样清空，再用这一帧重新打开一秒。
- 收口时事件码：见过离开为 2，否则见过进入为 1，否则 0。然后把当前轨迹集合存成上一秒。
- 全屋 `filled` 第一次到 10：`on_snapshot` 返回 true。之后每次再收口，计数加 1，到 2 就再返回 true 并清零这个计数。清空时这个计数和「已经做过第一次推理」一起归零。
- `zones()` 把 `filled==10` 的区按 `zone_id` 从小到大拷出，并清掉触发标志。环境行和 `epoch_sec` 各区相同。
- `event()` 在 `last_class` 已经等于 `class_index` 时返回 false。否则填 `peer_event_t`：`event_type` 用 `peer_evt_wire_name(PEER_EVT_ID_RESTING + class_index)`，`env_ch` 全 0，`zone_id` 和 `purpose` 用窗口里的，人数和五路 ×10、`room_type` 用收口最新一秒的快照原值，`targets[]` 只留该秒最后一帧可计数且属于该区的人。不修改 `last_class`。
- `event_sent()` 才把 `last_class` 写成这个类。
- `invalidate()` 与环境失败、秒号跳变用同一个清空函数。

每个对外函数、`zone` 结构体字段，以及上面每一条分支，按 Global Constraints 写中文注释。

- [ ] **Step 4: 跑窗口测试**

Expected: `ok`。

- [ ] **Step 5: 编进组件**

`components/peer_model/CMakeLists.txt`：

```cmake
idf_component_register(SRCS "peer_model_window.c"
                    INCLUDE_DIRS "include"
                    REQUIRES peer_event peer_frame)
```

- [ ] **Step 6: Commit**

```text
git add components/peer_model
git commit -m "feat: bucket snapshots into per-zone ten-second windows"
```

---

### Task 4: 搬入模型源码

**Files:**
- Create: `components/peer_model/vendor/` 下的副本
- Create: `components/peer_model/idf_component.yml`
- Modify: `components/peer_model/CMakeLists.txt`

**Interfaces:**
- Produces: `fusion_sensor_encode`、`fusion_classify`、`hamic_frontend_16k`、`hamic_pcm16_to_float`、`hamic_esp_dsp_fft_init`、`hamic_esp_dsp_fft512`、`fusion_model::audio_encoder_tflite`

- [ ] **Step 1: 复制文件**

从 `C:\Users\13348\Desktop\board_1009\board_1009` 复制，不改内容：

- `main/fusion_native.cc`、`main/fusion_native.h` → `components/peer_model/vendor/`
- `assets/fusion_model.h` → `components/peer_model/vendor/`
- `frontend/hamic_frontend.c`、`hamic_frontend.h`、`hamic_tables.c`、`hamic_tables.h`、`esp_dsp_fft.c`、`esp_dsp_fft.h` → `components/peer_model/vendor/`

不复制 `wav_reader`、`main.cc`、`demo_sensor_input.h`。

- [ ] **Step 2: 声明依赖并编进组件**

`components/peer_model/idf_component.yml`：

```yaml
dependencies:
  espressif/esp-tflite-micro: "==1.4.0"
  espressif/esp-dsp: "==1.8.2"
  espressif/esp-nn: "==1.3.2"
```

`CMakeLists.txt` 的 `SRCS` 加上 `vendor/fusion_native.cc`、`vendor/hamic_frontend.c`、`vendor/hamic_tables.c`、`vendor/esp_dsp_fft.c`。`INCLUDE_DIRS` 加上 `vendor`。`REQUIRES` 加上 `espressif__esp-tflite-micro`、`espressif__esp-dsp`、`espressif__esp-nn`。

- [ ] **Step 3: 确认窗口测试仍能过**

Task 3 的 `gcc` 命令不要把 `vendor` 加进去。Expected: `ok`。

- [ ] **Step 4: Commit**

```text
git add components/peer_model/vendor components/peer_model/idf_component.yml components/peer_model/CMakeLists.txt
git commit -m "feat: import the fusion model sources"
```

---

### Task 5: 麦环、快照队列和推理任务

**Files:**
- Create: `components/peer_model/include/peer_model.h`
- Create: `components/peer_model/peer_model.cc`
- Modify: `components/peer_model/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3 的窗口函数、Task 4 的模型函数、`peer_judge_push`、`es7210_mic_init` / `es7210_mic_read`
- Produces:
  - `void peer_model_start(void)`
  - `void peer_model_submit(const peer_snapshot_t *snap)` — 拷贝后返回。队列满则丢掉这一帧并 `peer_model_window_invalidate`，不挤掉队列里已有的快照。

- [ ] **Step 1: 写运行时**

`peer_model.h` 用 `extern "C"` 包住两个函数。注释写明：`submit` 只在串口回调里调用；推理在 `peer_model` 自己的任务里。

`peer_model.cc`：

- 快照队列深 48，元素是 `peer_snapshot_t`。`submit` 用 0 超时 `xQueueSend`。失败时把溢出标志设上，并 `xTaskNotifyGive` 模型任务。
- 模型任务优先处理溢出：丢掉队列里现有快照，调用 `peer_model_window_invalidate`，再清标志。清标志之后才收到的快照留在队列里。
- 正常路径：`xQueueReceive` 超时 100 ms。每取出一帧就 `peer_model_window_on_snapshot`。返回 true 时立刻推理，不要先把队列抽干。
- 推理：`peer_model_window_zones`。一个满窗的区都没有就结束这一拍。有区但左声道环还不满 160000 点，也结束这一拍，不调用 `event`。
- 环：PSRAM 上 16×16000 个 int16。麦任务持续 `es7210_mic_read`，立体声交错里只写左声道（偶数下标）。写环和拷 10 秒时用同一把互斥。拷贝只包住 `memcpy`，HAMIC 和 TFLite 在锁外。
- 麦任务：引脚与现在 `main` 里相同（SDA 1、SCL 2、SDO 3、BCLK 4、LRCK 5、MCLK 6）。`es7210_mic_init` 失败就 2 秒后重试，不用 `ESP_ERROR_CHECK`。读长保持 256 个立体声采样（1024 字节）。优先级 5，和原来的麦任务一样。
- 声音：`hamic_pcm16_to_float`，再 `hamic_frontend_16k(..., hamic_esp_dsp_fft512)`。输入张量是 HAMIC_FEATURES 个 float，输出 96 个 float。算子注册与 `board_1009/main/main.cc` 相同：Pad、Conv2D、DepthwiseConv2D、Concatenation、AveragePool2D、Reshape。arena 4×1024×1024，16 字节对齐，`MALLOC_CAP_SPIRAM`。`FusionSensorWorkspace` 也放 PSRAM。
- 每个区：`FusionSensorInput` 填该区 `radar`、共享 `env`、`epoch_sec`、`zone`、`room`。`fusion_sensor_encode` 失败或概率非有限就跳过该区。否则 softmax，`peer_model_top_class`，`peer_model_window_event` 为 true 时 `peer_judge_push`，只有 push 返回 true 才 `peer_model_window_event_sent`。
- 前端或 `Invoke` 失败：这一拍不再处理剩余区，不改已经成功 push 的区。注释写明剩余区的上一拍类别保持原样。
- 模型任务优先级 3，栈 16384。低于麦任务，这样推理时 I2S 仍能被读走。
- 初始化失败打一条 `ESP_LOGE`，任务返回前不推理。标签用 `peer_model`。

新写的函数和分支用中文注释。vendor 里的文件不要重新注释。

- [ ] **Step 2: 把 `peer_model.cc` 加进 SRCS**

`REQUIRES` 加上 `es7210_mic`、`freertos`、`log`。`peer_event` 已经在 REQUIRES 里。

- [ ] **Step 3: 再跑 PC 窗口测试**

Expected: `ok`。这个任务没有新的 PC 断言；推理路径靠 Task 6 的固件编译。

- [ ] **Step 4: Commit**

```text
git add components/peer_model/include/peer_model.h components/peer_model/peer_model.cc components/peer_model/CMakeLists.txt
git commit -m "feat: run fusion inference off the uart task"
```

---

### Task 6: 接到 main，并改 README 里已经变了的数字

**Files:**
- Modify: `main/SEN0766_P4_TJ.c`
- Modify: `main/CMakeLists.txt`
- Modify: `README.md`

**Interfaces:**
- Consumes: `peer_model_start`、`peer_model_submit`

- [ ] **Step 1: 改 main**

删除 `FRAME_SAMPLES`、`MIC_QUEUE_LEN`、`mic_frame_t`、`s_queue`、`mic_capture_task` 以及 `app_main` 里创建队列和麦任务的代码。去掉因此不再使用的头。

`on_peer_rx` 在 `peer_event_on_rx` 之后：若 `msg_type == PEER_MSG_SNAPSHOT` 且 `len == sizeof(peer_snapshot_t)`，把 payload 转成 `peer_snapshot_t` 再 `peer_model_submit`。注释写明 payload 只在回调期间有效，所以 submit 内部会拷贝。

`app_main` 在 `peer_slave_start` 之后调用 `peer_model_start`。自检失败因而没启动串口时，仍然启动模型任务（原来的麦任务也是这样无条件启动的）。`esp_log_level_set("peer_model", ESP_LOG_INFO)`。

`main/CMakeLists.txt` 的 `REQUIRES` 加上 `peer_model`。

- [ ] **Step 2: 改 README 里会说错的三处**

- 队列「最多 8 条」改为 24 条。满了仍丢最新的。
- 事件表在 6 `environment_recovered` 后加上 7–12 的六个类名。说明它们只在该类相对该区上一拍变化时出现，帧长仍是 136。
- 「怎么串起来」里补一句：`on_peer_rx` 在事件组件之后，若是 `0x10`，再交给 `peer_model_submit`。麦由 `peer_model` 独占。

不要在 README 里重写整份规格。

- [ ] **Step 3: 跑两个 PC 自检**

`test_peer_judge.exe` 和 `test_peer_model_window.exe` 都打印 `ok`。

- [ ] **Step 4: 编固件**

```text
idf.py build
```

Expected: 退出码 0。`peer_model` 被编进去，且没有第二个地方再调用 `es7210_mic_init`。

- [ ] **Step 5: Commit**

```text
git add main/SEN0766_P4_TJ.c main/CMakeLists.txt README.md
git commit -m "feat: feed live snapshots and the left mic into fusion"
```
