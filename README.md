# 组件说明

ESP32-P4 从机侧的组件。`peer_frame` 管帧格式，`peer_slave` 管 UART，`peer_event` 管进出房、进出区和环境边沿，`peer_model` 管分区融合推理，`es7243e_mic` 管单声道麦。第三方代码在 `managed_components`，不在这里。

依赖关系：`main` → `peer_slave`、`peer_event`、`peer_model`。`peer_model` 独占麦克风。`peer_slave` 和 `peer_event` 都依赖 `peer_frame`。事件判定不打开串口，串口不解释快照。

## 怎么串起来

`app_main` 里先跑自检，再把事件组件的四个函数填进从机回调：

```c
static void on_peer_rx(uint8_t msg_type, const uint8_t *payload, uint16_t len, uint8_t seq, void *user)
{
    peer_event_on_rx(msg_type, payload, len, seq, user);
    if (msg_type == PEER_MSG_SNAPSHOT && len == sizeof(peer_snapshot_t)) {
        peer_snapshot_t snap;
        memcpy(&snap, payload, sizeof(snap));
        peer_model_submit(&snap);
    }
}

static const peer_slave_sink_t sink = {
    .on_rx = on_peer_rx,
    .on_idle = peer_event_on_idle,
    .reply_payload = peer_event_reply_payload,
    .reply_done = peer_event_reply_done,
};
peer_event_selfcheck();
peer_slave_start(&sink);
```

主机（S3）周期做三件事：有待发配置就先发 `0x11`，再发 `0x10`，然后等 50 ms。从机不在这个节拍里阻塞。收到 `0x10` 后，队列里有事件才主动发 `0x20`。

`on_rx` 里的 `payload` 只在回调返回前有效。`0x10` 在事件组件之后交给 `peer_model_submit`，由它拷贝。麦由 `peer_model` 独占，不要再从 `main` 读 I2S。不要从事件组件里直接写 UART。

## 接线

| 信号 | P4 | 对端 |
|------|----|------|
| 调试 TX | GPIO25 | USB 转串口 RX |
| 业务 TX | GPIO30 | S3 RX |
| 业务 RX | GPIO29 | S3 TX |
| 麦 | GPIO1–6 | ES7243E |
| 音频导出 TX | GPIO24 | USB 转串口 RX，共地 |

业务口默认 `UART_NUM_1`，8N1，无流控，3 Mbps。调试日志走 `UART0` TX（GPIO25），115200，8N1，只出不进。不要把业务口改成 `UART0`。CRC 在短线上频繁失败时，只把 `PEER_UART_BAUD` 改成 `921600`，帧格式不变。业务口引脚和波特率是头文件宏；调试口在 `sdkconfig`。音频导出走 `UART_NUM_2` TX（GPIO24），921600，8N1，只出不进。`PEER_MODEL_AUDIO_DUMP` 在 `peer_model.cc`，默认 1。电脑先 `pip install pyserial`，再执行 `python tools/pcm_dump_wav.py COMx`。

## peer_frame

公共帧，小端，`pack(1)`，版本固定 `0x01`。

```
A5 5A | ver | msg_type | seq | len(u16 LE) | payload | crc16(LE)
```

`len` 只计 payload。CRC 是 Modbus（反射多项式 `0xA001`，初值 `0xFFFF`），从 `ver` 算到 payload 末尾，不含魔数和 CRC 自身。测试向量 `"123456789"` 的 CRC 为 `0x4B37`。

| msg_type | 方向 | 载荷 | 长度 |
|----------|------|------|------|
| `0x10` | S3 → P4 | `peer_snapshot_t` 瞬时雷达和环境 | 393 |
| `0x11` | S3 → P4 | `peer_env_cfg_t` 全机一份阈值 | 26 |
| `0x20` | P4 → S3 | `peer_event_t` 一条事件 | 200 |

长度不对就丢，包括旧的 118、136、144 字节事件帧。没有空事件帧，没有 SPI 两拍，没有反向 OTA。每条轨迹摘要是轨迹号、`motion`、横坐标、纵坐标。`motion` 由从机按 `0x10` 的 `kinesia` 写成 `still`、`micro`、`slow`、`fast`，分不出档则为空串。

常用函数：

- `peer_frame_encode`：组一帧，失败返回 0。
- `peer_frame_decode`：缓冲必须从 `A5 5A` 开始。成功时 payload 指向缓冲内部。
- `peer_frame_next`：从 UART 字节流里取下一帧。`*drop == 0` 表示后面还不够一帧，先留着；CRC 错只丢掉魔数的第一字节，避免把后面真正的帧头吃掉。

环境失败记 `-1`。温度、湿度、声级在线上已经是 ×10（30.0℃ 为 300）。`sound_db_x10` 不是校准 SPL。`utc_unix_s` 未对时不可信，边沿按帧先后，不按这个时间。

`0x11` 的 `mask`：bit0 CO₂、bit1 温度、bit2 湿度、bit3 照度、bit4 声级。界值 `-1` 表示该界不用。温度只有上限。

## peer_slave

UART 收发。扫到 `0x10` / `0x11` 后调用 `on_rx`。`0x10` 处理完立刻把队列里的事件组成 `0x20` 发出去，写完才调用 `reply_done`。`0x11` 不应答。

超过 `PEER_UART_IDLE_MS`（默认 2000 ms）没有合法 `0x10`，调用一次 `on_idle`。主机烧录本板、长时间停发时，靠这个空窗冻结占用，从机没有单独的 pause 接口。

发送路径有互斥，避免以后别的任务和 `0x20` 把字节交错在一起。

换引脚或波特率时，在包含 `peer_slave.h` 之前定义对应宏，或直接改头文件里的默认值。

## peer_event

把合法的 `0x10` / `0x11` 交给判定，再把队头交给从机组 `0x20`。

| 函数 | 作用 |
|------|------|
| `peer_event_on_rx` | 长度必须是 393 或 26，否则丢掉 |
| `peer_event_on_idle` | 主机停发，冻结占用 |
| `peer_event_reply_payload` | 拷贝队头，不出队。没有事件返回 0 |
| `peer_event_reply_done` | `0x20` 已经写入 UART 后出队，并打印一行事件日志 |
| `peer_event_selfcheck` | 尺寸、CRC、边沿规则。失败不要启动从机 |

`0x11` 收到后先排队，从下一帧 `0x10` 起用新阈值。

### 占用

进出房只看 `people_room`：上一拍为 0、这一拍大于 0 报 `room_entry`；反过来报 `room_exit`。

进出区看每条轨迹的 `track_id` 和 `zone_id`。`zone_id == 0` 不是业务区。

- 第一帧，以及空窗或雷达恢复后的第一帧，只记住人和区，不报。
- `radar_ready == 0` 或长时间没有 `0x10`：冻结占用，不因人数变 0 报离房、出区。恢复后的第一帧是新基线。
- 出区：上一帧某轨迹在某区，这一帧同一 `track_id` 已不在该区（换区、走到区外或轨迹消失）。
- 进区：这一帧某轨迹在某区，上一帧同一 `track_id` 不在该区。第一次进入也报 `zone_return`。
- 同一帧、同一个区只报一条，即使有多个人同时进出。区内还留着别人，离开的那个人仍会触发 `zone_exit`。
- 从 3 区走到 5 区：同一拍先 `zone_exit(3)`，再 `zone_return(5)`。

子区事件的 `targets[]` 只留这一帧仍在该区的人。房间级事件带检测范围内的全部目标。占用事件的 `env_ch` 全 0。

### 环境

五路各自边沿，名称是 `co2`、`temp`、`rh`、`lux`、`sound`。

- 越阈：CO₂、温度、声级为读数大于上限；湿度、照度为低于下限或高于上限。等于阈值不报。
- 恢复必须退回一个回差，避免在阈值上抖动连报。
- 读数 `-1`、`mask` 关掉的路：这一拍不报。
- 已经在坏区里不连报。
- 开机时已经超阈也会报一次。重复收到相同的 `0x11`、空窗、雷达未就绪，都不会把环境边沿清掉。

还没收到 `0x11` 时用文档默认：30℃、湿度 20–80%、CO₂ 1000 ppm、照度 50/1000、声级 60 dB，`mask=0x1F`。回差为 CO₂ 50、温度 0.5℃、湿度 2%、照度 20、声级 3 dB。主机把回差发成 0 时，用这些默认回差。

本轮事件名：

| 编号 | event_type |
|------|------------|
| 1 | `room_entry` |
| 2 | `room_exit` |
| 3 | `zone_exit` |
| 4 | `zone_return` |
| 5 | `environment_threshold_crossed` |
| 6 | `environment_recovered` |
| 7 | `resting` |
| 8 | `sleep` |
| 9 | `resting_bed` |
| 10 | `sleep_in_night_bed` |
| 11 | `wake_up_in_night_bed` |
| 12 | `empty_bed` |
| 13 | `work_office` |

当前模型第一名只有 `resting_bed`、`sleep_in_night_bed`、`work_office`。只在该类相对该区上一拍变化时出现，帧长是 200。7、8、11、12 仍留在表里。新增事件时在 `peer_event_map` 表尾加一行，编号不要复用。线上英文名只从这张表取。

队列最多 24 条。满了丢掉最新的一条。每条都是完整的 200 字节 `0x20`，一帧 `0x10` 里可以连续发出多条。相同 `event_type`、`zone_id`、`env_ch` 在 30 秒内只入队第一条，满 30 秒后的下一条照常入队。

自检可以在 PC 上跑，不烧板：

```text
gcc -std=c11 -I components/peer_frame/include -I components/peer_event/include ^
  components/peer_frame/peer_frame.c components/peer_event/peer_event_map.c ^
  components/peer_event/peer_judge.c components/peer_event/test_peer_judge.c ^
  -o test_peer_judge.exe
test_peer_judge.exe
```

打印 `ok` 即通过。秒窗口另有一条，同样不链接模型：

```text
gcc -std=c11 -I components/peer_frame/include -I components/peer_event/include -I components/peer_model/include ^
  components/peer_frame/peer_frame.c components/peer_event/peer_event_map.c ^
  components/peer_model/peer_model_window.c components/peer_model/test_peer_model_window.c ^
  -o test_peer_model_window.exe
test_peer_model_window.exe
```

## 编译

`es7243e_mic` 依赖 `espressif/esp_codec_dev`。融合模型是三类原生 C，不再依赖 TFLite Micro、ESP-DSP、ESP-NN。`managed_components/` 和 `build/` 不入库，第一次编译会重新下载：

```text
idf.py set-target esp32p4
idf.py build
```

目标芯片已写在 `sdkconfig` 里。本机 ESP-IDF 路径不要提交。

## es7243e_mic

单声道采集，和业务 UART 无关。P4 做 I2S 主机，输出 MCLK、BCLK、LRCK；ES7243E 做从机，从 SDO 送 PCM。I2C 只写寄存器，8 位地址按芯片默认（CE = 0，即 `0x20`）。

采样 16 kHz、16 bit、单声道。标准 I2S 总线仍是左右两槽，DMA 只收左槽。`es7243e_mic_read` 读出的是单声道 `int16`，`len` 必须是 2 的倍数。

```c
static const es7243e_mic_pins_t pins = {
    .sda = GPIO_NUM_1,
    .scl = GPIO_NUM_2,
    .sdo = GPIO_NUM_3,
    .bclk = GPIO_NUM_5,
    .lrck = GPIO_NUM_4,
    .mclk = GPIO_NUM_6,
};
es7243e_mic_init(&pins);
es7243e_mic_read(pcm, sizeof(pcm));
es7243e_mic_deinit();
```

未接芯片时 `es7243e_mic_init` 会失败，调用方应隔一段时间重试，不要用会复位的 `ESP_ERROR_CHECK` 包住它。重复 `init` 返回 `ESP_ERR_INVALID_STATE`。增益在打开采集之后设到 37.5 dB，这是芯片 PGA 的上限。
