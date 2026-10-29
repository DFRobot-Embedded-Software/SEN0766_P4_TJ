#include "peer_frame.h"

#include <string.h>

void peer_motion_name(uint8_t kinesia, char dst[8])
{
    const char *name = "";
    if (kinesia == 1) {
        name = "still";
    } else if (kinesia >= 2 && kinesia <= 30) {
        name = "micro";
    } else if (kinesia >= 31 && kinesia <= 60) {
        name = "slow";
    } else if (kinesia >= 61 && kinesia <= 100) {
        name = "fast";
    }
    memset(dst, 0, 8);
    memcpy(dst, name, strlen(name));
}

/**
 * Modbus CRC16。文档写 poly 0x8005，这里按 LSB 先算，用反射多项式 0xA001。
 * 初值 0xFFFF。主机测试向量 "123456789" 的结果为 0x4B37。
 */
uint16_t peer_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/** 组一帧写入 out。成功返回总字节数（7+payload+2），失败返回 0。 */
size_t peer_frame_encode(uint8_t msg_type, uint8_t seq, const uint8_t *payload, uint16_t payload_len,
                         uint8_t *out, size_t out_max)
{
    size_t total = 7u + (size_t)payload_len + 2u;
    if (out == NULL || total > out_max || total > PEER_FRAME_MAX) {
        return 0;
    }
    if (payload_len > 0 && payload == NULL) {
        return 0;
    }

    out[0] = PEER_FRAME_MAGIC0;
    out[1] = PEER_FRAME_MAGIC1;
    out[2] = PEER_FRAME_VER;
    out[3] = msg_type;
    out[4] = seq;
    out[5] = (uint8_t)(payload_len & 0xFF);
    out[6] = (uint8_t)(payload_len >> 8);
    if (payload_len > 0) {
        memcpy(&out[7], payload, payload_len);
    }
    uint16_t crc = peer_crc16(&out[2], 5u + (size_t)payload_len);
    out[7 + payload_len] = (uint8_t)(crc & 0xFF);
    out[8 + payload_len] = (uint8_t)(crc >> 8);
    return total;
}

/** buf 必须从 magic 开始。成功时 payload 指向 buf 内部，调用方不要释放。 */
bool peer_frame_decode(const uint8_t *buf, size_t buf_len, uint8_t *msg_type, uint8_t *seq,
                       const uint8_t **payload, uint16_t *payload_len)
{
    if (buf == NULL || buf_len < 9 || msg_type == NULL || seq == NULL ||
        payload == NULL || payload_len == NULL) {
        return false;
    }
    if (buf[0] != PEER_FRAME_MAGIC0 || buf[1] != PEER_FRAME_MAGIC1 || buf[2] != PEER_FRAME_VER) {
        return false;
    }
    uint16_t len = (uint16_t)buf[5] | ((uint16_t)buf[6] << 8);
    size_t total = 9u + (size_t)len;
    if (buf_len < total || total > PEER_FRAME_MAX) {
        return false;
    }
    uint16_t got = (uint16_t)buf[7 + len] | ((uint16_t)buf[8 + len] << 8);
    if (got != peer_crc16(&buf[2], 5u + (size_t)len)) {
        return false;
    }
    *msg_type = buf[3];
    *seq = buf[4];
    *payload_len = len;
    *payload = (len > 0) ? &buf[7] : NULL;
    return true;
}

/** 主机读回拍可能带前导 0，从前往后找第一帧合法 A5 5A。 */
bool peer_frame_scan_decode(const uint8_t *buf, size_t buf_len, uint8_t *msg_type, uint8_t *seq,
                            const uint8_t **payload, uint16_t *payload_len)
{
    if (buf == NULL || buf_len < 9) {
        return false;
    }
    for (size_t i = 0; i + 9 <= buf_len; i++) {
        if (buf[i] == PEER_FRAME_MAGIC0 && buf[i + 1] == PEER_FRAME_MAGIC1 &&
            peer_frame_decode(buf + i, buf_len - i, msg_type, seq, payload, payload_len)) {
            return true;
        }
    }
    return false;
}

/**
 * 流式拆帧。
 * 先跳过不是 0xA5 的字节。看到 A5 但下一字节还没到，停住不丢。
 * 下一字节不是 5A，这个 A5 当垃圾，继续往后。
 * 帧头齐了但总长还没到齐，停住，*drop 指到帧头，调用方不要丢这段。
 * len 大得装不下一帧，这个 A5 当垃圾。
 * 长度够但 CRC/版本不对：*drop 只加 1，下一轮从 A5 的下一字节再找，避免帧内还有一个真帧头被整段扔掉。
 */
bool peer_frame_next(const uint8_t *buf, size_t buf_len, size_t *drop, uint8_t *msg_type,
                     uint8_t *seq, const uint8_t **payload, uint16_t *payload_len)
{
    if (drop == NULL) {
        return false;
    }
    *drop = 0;
    if (buf == NULL || buf_len == 0) {
        return false;
    }

    size_t i = 0;
    while (i < buf_len) {
        if (buf[i] != PEER_FRAME_MAGIC0) {
            i++;
            continue;
        }
        if (i + 1 >= buf_len) {
            *drop = i;
            return false;
        }
        if (buf[i + 1] != PEER_FRAME_MAGIC1) {
            i++;
            continue;
        }
        if (i + 9 > buf_len) {
            *drop = i;
            return false;
        }
        uint16_t len = (uint16_t)buf[i + 5] | ((uint16_t)buf[i + 6] << 8);
        size_t total = 9u + (size_t)len;
        if (len > PEER_FRAME_MAX - 9u || total > PEER_FRAME_MAX) {
            i++;
            continue;
        }
        if (i + total > buf_len) {
            *drop = i;
            return false;
        }
        if (peer_frame_decode(buf + i, buf_len - i, msg_type, seq, payload, payload_len)) {
            *drop = i;
            return true;
        }
        *drop = i + 1;
        return false;
    }
    *drop = buf_len;
    return false;
}
