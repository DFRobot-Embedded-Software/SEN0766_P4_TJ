idf_component_register(SRCS "es7243e_mic.c"
                    INCLUDE_DIRS "include"
                    REQUIRES espressif__esp_codec_dev esp_driver_i2c esp_driver_i2s esp_driver_gpio)
