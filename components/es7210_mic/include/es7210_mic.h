#pragma once

/**
 * ES7210 双麦采集。
 * ESP32-P4 做 I2S 主机，输出 MCLK/BCLK/LRCK；ES7210 做从机，SDO 送交错立体声 PCM。
 * I2C 只用来写寄存器。麦脚在 GPIO1–6，和业务 UART 分开。
 * 采样 16 kHz、16 bit、左右声道各一路。增益在 open 之后设到芯片上限，避免被驱动默认值盖掉。
 */

#include <stddef.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 采样率，单位 Hz。双麦标准 I2S，左右声道各一路。 */
#define ES7210_MIC_SAMPLE_RATE_HZ  16000
/** 每个采样点的位宽。立体声一帧 = 左 16 bit + 右 16 bit。 */
#define ES7210_MIC_BITS            16
/** 声道数：MIC1 为左声道，MIC2 为右声道。 */
#define ES7210_MIC_CHANNELS        2
/**
 * ES7210 模拟 PGA 的最大增益，单位 dB。
 * 芯片增益表最后一档是 37.5 dB，再大的数值也会被驱动收成这一档。
 */
#define ES7210_MIC_GAIN_MAX_DB     37.5f

/**
 * 板级引脚。
 * SDO 是 ES7210 的数据输出，接到 ESP32-P4 的 I2S DIN。
 * LRCK 即 I2S 的 WS（字选择）。
 */
typedef struct {
    gpio_num_t sda;   /* I2C 数据线，配置 ES7210 寄存器 */
    gpio_num_t scl;   /* I2C 时钟线 */
    gpio_num_t mclk;  /* I2S 主时钟，由 ESP32-P4 输出，频率 = 采样率 × 256 */
    gpio_num_t bclk;  /* I2S 位时钟 */
    gpio_num_t lrck;  /* I2S 左右声道时钟（WS） */
    gpio_num_t sdo;   /* ES7210 串行数据输出 */
} es7210_mic_pins_t;

/**
 * 初始化 I2C、标准 I2S 接收通道和 ES7210，并打开双麦采集。
 * 成功返回后增益已设为 ES7210_MIC_GAIN_MAX_DB。
 * 重复调用返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t es7210_mic_init(const es7210_mic_pins_t *pins);

/**
 * 读取一帧交错的立体声 PCM：int16 小端，左声道在前。
 * len 必须是 4 的倍数（一个立体声采样占 4 字节）。
 * 未初始化时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t es7210_mic_read(void *buf, size_t len);

/** 关闭采集并释放 I2S、I2C。未初始化时直接返回 ESP_OK。 */
esp_err_t es7210_mic_deinit(void);

#ifdef __cplusplus
}
#endif
