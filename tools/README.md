# pcm_dump_wav.py

把 P4 送给融合模型的那 10 秒音频收下来，存成 WAV，用来听麦克风是否正常。

只收推理实际用到的一段，不是连续麦流。采样 16 kHz、单声道、16 bit，160000 个采样点，时长 10 秒。

## 接线

音频导出和调试日志、业务口不是同一路。

| 信号 | P4 | 电脑 |
|------|----|------|
| 音频导出 TX | GPIO24（UART2） | USB 转串口 RX |
| 地 | GND | GND |

921600，8N1，无流控，只发不收。调试日志仍走 GPIO25，115200，不要接到这一路。

固件里 `peer_model.cc` 的 `PEER_MODEL_AUDIO_DUMP` 默认是 1。改成 0 后这段代码不编译，UART2 也不会打开。

## 安装

需要 Python 3 和 pyserial。`wave` 是标准库，不用另装。

```text
pip install pyserial
```

## 使用

在仓库根目录执行，把 `COMx` 换成 USB 转串口的端口。Windows 可在设备管理器里看端口号。

```text
python tools/pcm_dump_wav.py COM5
python tools/pcm_dump_wav.py COM5 --out D:\wav
```

`--out` 省略时，WAV 写在当前目录。文件名是 `dump-001.wav`、`dump-002.wav`，已有同名文件就往后编号。每存好一段，终端打印该文件路径。

推理跑起来之后才会出一帧。按 Ctrl+C 结束。

## 帧

```text
PCM1 | 采样率 uint32 小端 | 采样点数 uint32 小端 | PCM int16 小端
```

采样率必须是 16000，点数必须是 160000。不对则丢掉这个头，从下一个字节继续找 `PCM1`。超过 1 秒没有新字节时，未收完的半帧丢掉，重新对齐。没有 CRC。

## 自检

不打开串口，检查组帧和半帧丢弃。

```text
python tools/pcm_dump_wav.py --self-check
```

通过时进程退出码为 0。
