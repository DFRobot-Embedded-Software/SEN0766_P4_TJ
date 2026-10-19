#pragma once

/**
 * ES7243E 单声道采集。
 * ESP32-P4 做 I2S 主机，输出 MCLK/BCLK/LRCK；ES7243E 做从机，SDO 送 PCM。
 * 标准 I2S 总线仍是左右两槽，DMA 只收左槽，读出的是单声道 PCM16。
 * I2C 只用来写寄存器。麦脚在 GPIO1–6，和业务 UART 分开。
 * 采样 16 kHz、16 bit、单声道。增益在 open 之后设到芯片上限，避免被驱动默认值盖掉。
 */

#include <stddef.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 采样率，单位 Hz。 */
#define ES7243E_MIC_SAMPLE_RATE_HZ  16000
/** 每个采样点的位宽。单声道一帧 = 16 bit。 */
#define ES7243E_MIC_BITS            16
/** 声道数。总线有左右两槽，只收下左槽。 */
#define ES7243E_MIC_CHANNELS        1
/**
 * ES7243E 模拟 PGA 的最大增益，单位 dB。
 * 芯片增益表最后一档是 37.5 dB，再大的数值也会被驱动收成这一档。
 */
#define ES7243E_MIC_GAIN_MAX_DB     37.5f

/**
 * 板级引脚。
 * SDO 是 ES7243E 的数据输出，接到 ESP32-P4 的 I2S DIN。
 * LRCK 即 I2S 的 WS（字选择）。
 */
typedef struct {
    gpio_num_t sda;   /* I2C 数据线，配置 ES7243E 寄存器 */
    gpio_num_t scl;   /* I2C 时钟线 */
    gpio_num_t mclk;  /* I2S 主时钟，由 ESP32-P4 输出，频率 = 采样率 × 256 */
    gpio_num_t bclk;  /* I2S 位时钟 */
    gpio_num_t lrck;  /* I2S 左右声道时钟（WS） */
    gpio_num_t sdo;   /* ES7243E 串行数据输出 */
} es7243e_mic_pins_t;

/**
 * 初始化 I2C、标准 I2S 接收通道和 ES7243E，并打开单声道采集。
 * 成功返回后增益已设为 ES7243E_MIC_GAIN_MAX_DB。
 * 重复调用返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t es7243e_mic_init(const es7243e_mic_pins_t *pins);

/**
 * 读取单声道 PCM16：int16 小端，一个采样 2 字节。
 * len 必须是 2 的倍数。
 * 未初始化时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t es7243e_mic_read(void *buf, size_t len);

/** 关闭采集并释放 I2S、I2C。未初始化时直接返回 ESP_OK。 */
esp_err_t es7243e_mic_deinit(void);

#ifdef __cplusplus
}
#endif
