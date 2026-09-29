# UART 从机。线格式在 peer_frame，事件在 peer_event。
idf_component_register(SRCS "peer_slave.c"
                    INCLUDE_DIRS "include"
                    REQUIRES peer_frame esp_driver_uart freertos esp_common)
