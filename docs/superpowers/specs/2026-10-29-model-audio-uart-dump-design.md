# 模型音频串口导出

把每次推理实际使用的 10 秒麦克风 PCM 从 GPIO24 送出。电脑脚本按帧收齐后存成 WAV，用来听麦是否正常。业务口、日志口、`0x10` / `0x20` 帧格式都不改。

## 何时送出

只在 `run_beat` 里，`copy_latest_audio` 已经成功之后、`hamic_pcm16_to_float` 之前。声环不满 10 秒时不送。送出的字节与当时的 `s_pcm` 相同：160000 个小端 `int16`，16 kHz，单声道。

模型任务阻塞写完再继续这一拍推理。麦任务不暂停，16 秒声环继续写入。写出大约 3.5 秒，快照队列可能因此溢出并清空窗口；这是诊断期间接受的后果。宏关掉后，节拍与现在相同。

## 串口

`peer_model_start` 里初始化一次，使用 `UART_NUM_2`。TX 为 GPIO24，RX 不接。921600，8N1，无流控。`peer_model` 的 CMake `REQUIRES` 加上 `esp_driver_uart`。初始化失败只打一条日志，之后不再导出，推理照常，麦任务照常创建。

`uart_write_bytes` 循环直到写完 12 字节头和 320000 字节 PCM。单次等待 1 秒，整段累计超过 5 秒视为失败。失败只打日志，这一拍推理照常。

## 线上格式

小端，无 CRC。

| 字段 | 内容 |
|------|------|
| 4 字节 | ASCII `PCM1` |
| `uint32` | 采样率 `16000` |
| `uint32` | 采样数 `160000` |
| 载荷 | 320000 字节 PCM |

对齐只靠 `PCM1`。宏 `PEER_MODEL_AUDIO_DUMP` 定义在 `peer_model.cc`，默认 `1`。为 `0` 时导出代码不编译，UART2 不打开。可用编译选项覆盖该宏。

## 电脑脚本

`tools/pcm_dump_wav.py`。标准库 `wave` 加 `pyserial`（`pip install pyserial`）。

```
python tools/pcm_dump_wav.py COMx
python tools/pcm_dump_wav.py COMx --out 目录
```

串口号必填。`--out` 省略时写到当前目录。脚本在字节流里找 `PCM1`，读出采样率和点数。采样率不是 16000，或点数不是 160000，整段丢掉，继续找下一个 `PCM1`。收满后写成 16 bit、单声道、16000 Hz 的 `dump-001.wav`，序号递增；同名文件已存在就用下一个序号。收满之前空闲超过 1 秒，丢掉已收字节，从缓冲区重新找 `PCM1`。

`--self-check` 不打开串口，在内存里解析。一段完整帧得到 10 秒 PCM，字节与载荷相同。一段写到一半就断、后面再跟一段完整帧时，丢掉前一段，后一段的字节与载荷相同。成功退出码 0，失败非 0。

## 文档

`README.md` 接线表加一行：音频导出 TX 是 GPIO24，对端是 USB 转串口 RX，共地。同节写上 `PEER_MODEL_AUDIO_DUMP`、921600，以及 `python tools/pcm_dump_wav.py COMx`。
