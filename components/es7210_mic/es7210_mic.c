#include "es7210_mic.h"

/**
 * 用 esp_codec_dev 包一层 ES7210。
 * 创建顺序：I2C 总线 → I2S 接收 → 数据接口 → I2C 控制接口 → codec → 设备。
 * 任一步失败都按相反顺序拆掉，避免下次 init 看到半初始化状态。
 */

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "es7210_adc.h"

static const char *TAG = "es7210_mic";

/** 芯片默认 8 位 I2C 地址（AD1/AD0 = 00）。驱动内部会右移成 7 位地址 0x40。 */
#define ES7210_I2C_ADDR_8BIT    ES7210_CODEC_DEFAULT_ADDR
#define ES7210_I2C_FREQ_HZ      400000

typedef struct {
    i2c_master_bus_handle_t      i2c_bus;
    i2s_chan_handle_t            rx;
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_data_if_t *data_if;
    const audio_codec_if_t      *codec_if;
    esp_codec_dev_handle_t       dev;
    bool                         ready;
} es7210_mic_ctx_t;

static es7210_mic_ctx_t s_mic;

/** 按与创建相反的顺序拆掉已经建好的资源，初始化失败时也能收回。 */
static void es7210_mic_release(void)
{
    if (s_mic.dev) {
        esp_codec_dev_close(s_mic.dev);
        esp_codec_dev_delete(s_mic.dev);
        s_mic.dev = NULL;
    }
    if (s_mic.codec_if) {
        audio_codec_delete_codec_if(s_mic.codec_if);
        s_mic.codec_if = NULL;
    }
    if (s_mic.data_if) {
        audio_codec_delete_data_if(s_mic.data_if);
        s_mic.data_if = NULL;
    }
    if (s_mic.ctrl_if) {
        audio_codec_delete_ctrl_if(s_mic.ctrl_if);
        s_mic.ctrl_if = NULL;
    }
    if (s_mic.rx) {
        /* open 成功后通道才处于 enable；close 会先关掉。未接芯片时通道从未 enable，这里再 disable 会报错。 */
        i2s_del_channel(s_mic.rx);
        s_mic.rx = NULL;
    }
    if (s_mic.i2c_bus) {
        i2c_del_master_bus(s_mic.i2c_bus);
        s_mic.i2c_bus = NULL;
    }
    s_mic.ready = false;
}

/** 建立访问 ES7210 寄存器的 I2C 主机。 */
static esp_err_t es7210_i2c_init(const es7210_mic_pins_t *pins)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = pins->sda,
        .scl_io_num = pins->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_mic.i2c_bus);
}

/**
 * 建立标准 I2S 接收通道。
 * ESP32-P4 做主机，产生 MCLK / BCLK / LRCK；ES7210 做从机，从 SDO 送出 PCM。
 * 通道先不要使能，等 esp_codec_dev_open() 里统一打开。
 */
static esp_err_t es7210_i2s_init(const es7210_mic_pins_t *pins)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_mic.rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ES7210_MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = pins->mclk,
            .bclk = pins->bclk,
            .ws = pins->lrck,
            .dout = I2S_GPIO_UNUSED,
            .din = pins->sdo,
        },
    };
    return i2s_channel_init_std_mode(s_mic.rx, &std_cfg);
}

esp_err_t es7210_mic_init(const es7210_mic_pins_t *pins)
{
    if (pins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mic.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = es7210_i2c_init(pins);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 初始化失败: %s", esp_err_to_name(err));
        es7210_mic_release();
        return err;
    }

    err = es7210_i2s_init(pins);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化失败: %s", esp_err_to_name(err));
        es7210_mic_release();
        return err;
    }

    /* 把已经建好的 I2S 接收通道交给编解码数据接口，后续 read 走这条通道。 */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .rx_handle = s_mic.rx,
        .tx_handle = NULL,
    };
    s_mic.data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_mic.data_if == NULL) {
        es7210_mic_release();
        return ESP_ERR_NO_MEM;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES7210_I2C_ADDR_8BIT,
        .bus_handle = s_mic.i2c_bus,
        .clock_speed_hz = ES7210_I2C_FREQ_HZ,
    };
    s_mic.ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_mic.ctrl_if == NULL) {
        es7210_mic_release();
        return ESP_ERR_NO_MEM;
    }

    /* 只打开 MIC1、MIC2。两路以下走标准 I2S，不会切到 TDM。 */
    es7210_codec_cfg_t codec_cfg = {
        .ctrl_if = s_mic.ctrl_if,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .mclk_src = ES7210_MCLK_FROM_PAD,
    };
    s_mic.codec_if = es7210_codec_new(&codec_cfg);
    if (s_mic.codec_if == NULL) {
        ESP_LOGE(TAG, "ES7210 创建失败，请检查 I2C 地址和接线");
        es7210_mic_release();
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_mic.codec_if,
        .data_if = s_mic.data_if,
    };
    s_mic.dev = esp_codec_dev_new(&dev_cfg);
    if (s_mic.dev == NULL) {
        es7210_mic_release();
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = ES7210_MIC_BITS,
        .channel = ES7210_MIC_CHANNELS,
        .sample_rate = ES7210_MIC_SAMPLE_RATE_HZ,
    };
    int codec_ret = esp_codec_dev_open(s_mic.dev, &fs);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "打开采集失败: %d", codec_ret);
        es7210_mic_release();
        return ESP_FAIL;
    }

    /*
     * open 内部会按默认增益（0 dB）再写一次寄存器。
     * 必须在 open 之后把增益设到芯片上限 37.5 dB，否则最大增益会被盖掉。
     */
    codec_ret = esp_codec_dev_set_in_gain(s_mic.dev, ES7210_MIC_GAIN_MAX_DB);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "设置最大增益失败: %d", codec_ret);
        es7210_mic_release();
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_mute(s_mic.dev, false);

    s_mic.ready = true;
    ESP_LOGI(TAG, "ES7210 双麦采集已启动，增益 %.1f dB，%u Hz",
             ES7210_MIC_GAIN_MAX_DB, ES7210_MIC_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t es7210_mic_read(void *buf, size_t len)
{
    if (buf == NULL || len == 0 || (len % (ES7210_MIC_CHANNELS * sizeof(int16_t))) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mic.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = esp_codec_dev_read(s_mic.dev, buf, (int) len);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t es7210_mic_deinit(void)
{
    es7210_mic_release();
    return ESP_OK;
}
