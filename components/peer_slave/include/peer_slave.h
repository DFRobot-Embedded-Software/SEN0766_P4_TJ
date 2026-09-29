#pragma once

/**
 * UART 从机（P4 侧）。
 *
 * 只做收发，不算进出房、不解释环境阈值。线格式在 peer_frame，边沿在 peer_event。
 * S3 周期发 0x11（有则先发）和 0x10；P4 扫到完整帧后回调上层。
 * 队列里有事件才主动发 0x20，长度必须是 sizeof(peer_event_t)（136）。不再发 len=0 空帧，也没有 SPI 两拍读回。
 *
 * 麦占用 GPIO1–6，业务 UART 避开这两脚。TX/RX 与 S3 交叉：本板 TX 接对端 RX。
 * 默认 3 Mbps、8N1、无流控。CRC 频繁失败时可把 PEER_UART_BAUD 改成 921600，帧格式不变。
 * 控制台占 UART0，业务口默认 UART1。
 *
 * 超过 PEER_UART_IDLE_MS 没有合法 0x10 时调用 on_idle 一次，直到下一帧 0x10 再重新计时。
 * 主机烧录 P4 时会停发 0x10/0x11，从机靠这个空窗冻结占用，不另做 pause 接口。
 */

#include "driver/uart.h"
#include "esp_err.h"
#include "peer_frame.h"

/** 业务 UART。不要用 UART0，那是日志口。 */
#ifndef PEER_UART_PORT
#define PEER_UART_PORT UART_NUM_1
#endif
/** 本板发送脚，接到 S3 的 RX。 */
#ifndef PEER_UART_TX
#define PEER_UART_TX 30
#endif
/** 本板接收脚，接到 S3 的 TX。 */
#ifndef PEER_UART_RX
#define PEER_UART_RX 29
#endif
/** 板对板短线默认 3 Mbps。 */
#ifndef PEER_UART_BAUD
#define PEER_UART_BAUD 3000000
#endif
/** 驱动环形缓冲。0x10 一帧约 402 字节，留足余量避免 FIFO 溢出。 */
#ifndef PEER_UART_RX_BUF
#define PEER_UART_RX_BUF 2048
#endif
/** 连续这么久没有合法 0x10，就认为主机停发，通知上层冻结占用。 */
#ifndef PEER_UART_IDLE_MS
#define PEER_UART_IDLE_MS 2000
#endif

/**
 * 上层回调。payload 只在 on_rx 返回前有效，要留用必须自己拷走。
 * reply_payload / reply_done 只在处理完一帧 0x10 之后、同一任务里调用。
 */
typedef struct {
    void *user;
    /** 一帧 CRC 通过的 0x10 或 0x11。其它 msg_type 不会进来。 */
    void (*on_rx)(uint8_t msg_type, const uint8_t *payload, uint16_t len, uint8_t seq, void *user);
    /** 长时间没有 0x10。重复空窗不会连着再调。 */
    void (*on_idle)(void *user);
    /**
     * 取一条待发事件的 payload（peer_event_t）。
     * 返回 136 表示有事件；返回 0 表示队列空。本函数不弹队列。
     */
    uint16_t (*reply_payload)(uint8_t seq, uint8_t *dst, uint16_t dst_max, void *user);
    /** 对应的 0x20 已经写入 UART 之后调用，上层此时才出队。 */
    void (*reply_done)(void *user);
} peer_slave_sink_t;

/** 装驱动、建互斥、拉起 peer_uart 任务。失败时不保证部分资源已回收。 */
esp_err_t peer_slave_start(const peer_slave_sink_t *sink);
