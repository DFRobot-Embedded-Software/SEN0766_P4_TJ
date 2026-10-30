# 模型音频串口导出 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把每次推理实际使用的 10 秒 PCM 从 GPIO24 送出，电脑脚本存成 WAV。

**Architecture:** `peer_model.cc` 在 `copy_latest_audio` 成功后阻塞写 UART2。帧是 `PCM1` 加两个小端 `uint32` 再加 PCM。`tools/pcm_dump_wav.py` 按这个帧切段，用标准库 `wave` 写文件。宏为 0 时导出代码不编译。

**Tech Stack:** ESP-IDF 5.5 `esp_driver_uart`、现有 `peer_model.cc`、Python 3 标准库 `wave` 加 `pyserial`。

## Global Constraints

- 规格：`docs/superpowers/specs/2026-10-29-model-audio-uart-dump-design.md`
- 只在 `copy_latest_audio` 成功之后、`hamic_pcm16_to_float` 之前送出。字节与当时的 `s_pcm` 相同：160000 个小端 `int16`。
- `UART_NUM_2`，TX GPIO24，RX 不接，921600，8N1，无流控。`peer_model_start` 里初始化一次。
- 头 12 字节：`PCM1`、采样率 `16000`、采样数 `160000`，小端，无 CRC。随后 320000 字节 PCM。
- 写失败或整段超过 5 秒：只打日志，这一拍推理照常。初始化失败同样只打日志。
- `PEER_MODEL_AUDIO_DUMP` 在 `peer_model.cc`，默认 `1`。为 `0` 时不编译导出、不打开 UART2。
- 业务口、日志口、`0x10` / `0x20` 不改。
- 脚本：`python tools/pcm_dump_wav.py COMx`，`--out` 可省略（当前目录）。空闲超过 1 秒丢掉未收完的字节。采样率或点数不对则丢掉该头并继续找 `PCM1`。
- `--self-check` 不打开串口。完整帧的 PCM 字节与载荷相同。半帧后空闲再跟完整帧时，只留下后一段。

## File Structure

- `tools/pcm_dump_wav.py`：解析、写 WAV、自检、串口循环。
- `components/peer_model/peer_model.cc`：宏、UART2 初始化、写出。
- `components/peer_model/CMakeLists.txt`：`REQUIRES` 加 `esp_driver_uart`。
- `README.md`：接线表和同节的用法。

---

### Task 1: 电脑端按 PCM1 切段并自检

**Files:**
- Create: `tools/pcm_dump_wav.py`

**Interfaces:**
- Consumes: 线上 12 字节头 + 320000 字节载荷
- Produces: `DumpParser.feed(data: bytes) -> list[bytes]`，`DumpParser.idle() -> None`，`pack_frame(rate: int, count: int, payload: bytes) -> bytes`

- [ ] **Step 1: Write the failing test**

`tools/pcm_dump_wav.py` 里先只放自检入口，引用还不存在的 `pack_frame` 和 `DumpParser`：

```python
def self_check():
    payload = bytes(i & 0xFF for i in range(320000))
    frame = pack_frame(16000, 160000, payload)
    ok = DumpParser().feed(frame)
    if ok != [payload]:
        return False
    parser = DumpParser()
    parser.feed(frame[:100])
    parser.idle()
    return parser.feed(frame) == [payload]


if __name__ == "__main__":
    import sys
    if "--self-check" in sys.argv:
        raise SystemExit(0 if self_check() else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python tools/pcm_dump_wav.py --self-check`
Expected: FAIL with `NameError`（`pack_frame` 或 `DumpParser` 未定义）

- [ ] **Step 3: Write minimal implementation**

补上解析、WAV 和串口循环。采样率或点数不对时只丢掉这 12 字节头的第一个字节，从后面继续找 `PCM1`。不按错误的点数去读载荷。

```python
import argparse
import struct
import wave
from pathlib import Path

MAGIC = b"PCM1"
RATE = 16000
COUNT = 160000
PCM_BYTES = COUNT * 2


def pack_frame(rate, count, payload):
    return MAGIC + struct.pack("<II", rate, count) + payload


class DumpParser:
    def __init__(self):
        self.buf = bytearray()

    def idle(self):
        self.buf.clear()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                if len(self.buf) > 3:
                    del self.buf[:-3]
                break
            del self.buf[:i]
            if len(self.buf) < 12:
                break
            rate, count = struct.unpack_from("<II", self.buf, 4)
            if rate != RATE or count != COUNT:
                del self.buf[:1]
                continue
            if len(self.buf) < 12 + PCM_BYTES:
                break
            out.append(bytes(self.buf[12:12 + PCM_BYTES]))
            del self.buf[:12 + PCM_BYTES]
        return out


def write_wav(path, pcm):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)


def next_wav(out_dir):
    n = 1
    while True:
        path = Path(out_dir) / f"dump-{n:03d}.wav"
        if not path.exists():
            return path
        n += 1


def self_check():
    payload = bytes(i & 0xFF for i in range(PCM_BYTES))
    frame = pack_frame(RATE, COUNT, payload)
    if DumpParser().feed(frame) != [payload]:
        return False
    parser = DumpParser()
    parser.feed(frame[:100])
    parser.idle()
    return parser.feed(frame) == [payload]


def main():
    if "--self-check" in sys.argv:
        raise SystemExit(0 if self_check() else 1)
    p = argparse.ArgumentParser()
    p.add_argument("port")
    p.add_argument("--out", default=".")
    args = p.parse_args()
    import serial
    ser = serial.Serial(args.port, 921600, timeout=1)
    parser = DumpParser()
    while True:
        chunk = ser.read(4096)
        if not chunk:
            parser.idle()
            continue
        for pcm in parser.feed(chunk):
            path = next_wav(args.out)
            write_wav(path, pcm)
            print(path)


if __name__ == "__main__":
    import sys
    main()
```

`self_check` 上面的临时入口删掉，只留 `main`。文件顶部保留 `import sys`，不要在 `main` 里再导入一次。

- [ ] **Step 4: Run test to verify it passes**

Run: `python tools/pcm_dump_wav.py --self-check`
Expected: 退出码 0，无输出

- [ ] **Step 5: Commit**

```bash
git add tools/pcm_dump_wav.py
git commit -m "让电脑按 PCM1 帧把模型音频存成 WAV"
```

---

### Task 2: 从机在推理前写出这 10 秒

**Files:**
- Modify: `components/peer_model/CMakeLists.txt`
- Modify: `components/peer_model/peer_model.cc`
- Modify: `README.md` 接线表（约第 38–45 行）

**Interfaces:**
- Consumes: `s_pcm`、`AUDIO_SAMPLES`（160000）、`copy_latest_audio`
- Produces: UART2 上的 `PCM1` 帧，布局与 `pack_frame(16000, 160000, pcm)` 相同

- [ ] **Step 1: Add the UART requirement**

`CMakeLists.txt` 的 `REQUIRES` 改为：

```
REQUIRES peer_event peer_frame es7243e_mic freertos log heap esp_driver_uart
```

- [ ] **Step 2: Dump after a full 10 second copy**

`peer_model.cc` 在现有 include 后加：

```cpp
#ifndef PEER_MODEL_AUDIO_DUMP
#define PEER_MODEL_AUDIO_DUMP 1
#endif

#if PEER_MODEL_AUDIO_DUMP
#include "driver/uart.h"
#endif
```

在 `run_beat` 之前加初始化、分块写出。TX 缓冲为 0，驱动会把这一块推进 FIFO 才返回。一块 4096 字节约 45 ms，短于规格里的单次 1 秒。整段从第一字节起算，超过 5 秒停止。

```cpp
#if PEER_MODEL_AUDIO_DUMP
static bool s_dump_on;

static void dump_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = 921600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(UART_NUM_2, 256, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(UART_NUM_2, &cfg) != ESP_OK ||
        uart_set_pin(UART_NUM_2, GPIO_NUM_24, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "音频串口初始化失败");
        return;
    }
    s_dump_on = true;
}

/** 一块 4096 字节约 45 ms。驱动无写超时，整段用 tick 卡 5 秒。 */
static bool dump_write(const void *src, size_t len, TickType_t t0)
{
    const uint8_t *p = static_cast<const uint8_t *>(src);
    size_t off = 0;
    while (off < len) {
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(5000)) {
            return false;
        }
        size_t n = len - off;
        if (n > 4096) {
            n = 4096;
        }
        int w = uart_write_bytes(UART_NUM_2, p + off, n);
        if (w <= 0) {
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
}

static void dump_pcm(const int16_t *pcm)
{
    if (!s_dump_on) {
        return;
    }
    uint8_t hdr[12];
    uint32_t rate = 16000;
    uint32_t count = AUDIO_SAMPLES;
    memcpy(hdr, "PCM1", 4);
    memcpy(hdr + 4, &rate, 4);
    memcpy(hdr + 8, &count, 4);
    TickType_t t0 = xTaskGetTickCount();
    if (!dump_write(hdr, sizeof(hdr), t0) ||
        !dump_write(pcm, AUDIO_SAMPLES * sizeof(int16_t), t0)) {
        ESP_LOGW(TAG, "音频导出失败");
    }
}
#endif
```

`run_beat` 里 `copy_latest_audio` 成功后立刻调用：

```cpp
    if (!copy_latest_audio(s_pcm)) {
        return;
    }
#if PEER_MODEL_AUDIO_DUMP
    dump_pcm(s_pcm);
#endif
```

`peer_model_start` 里队列和声环都申请成功、创建任务之前调用 `dump_init()`。初始化失败不 `return`，任务照常创建。

- [ ] **Step 3: Document the pin**

`README.md` 接线表加一行：`音频导出 TX` | `GPIO24` | `USB 转串口 RX，共地`。

表后的那段加上：音频导出走 `UART_NUM_2` TX（GPIO24），921600，8N1，只出不进。`PEER_MODEL_AUDIO_DUMP` 在 `peer_model.cc`，默认 1。电脑执行 `python tools/pcm_dump_wav.py COMx`（需要 `pyserial`）。

- [ ] **Step 4: Run the script self-check again**

Run: `python tools/pcm_dump_wav.py --self-check`
Expected: 退出码 0

固件没有主机侧串口测试。不在这里跑 `idf.py build`，除非当前 shell 已经导出 IDF。

- [ ] **Step 5: Commit**

```bash
git add components/peer_model/peer_model.cc components/peer_model/CMakeLists.txt README.md
git commit -m "推理前把 10 秒 PCM 从 GPIO24 送出，便于听麦"
```
