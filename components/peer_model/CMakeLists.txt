idf_component_register(SRCS "peer_model_window.c"
                         "vendor/fusion_native.cc"
                         "vendor/hamic_frontend.c"
                         "vendor/hamic_tables.c"
                         "vendor/esp_dsp_fft.c"
                    INCLUDE_DIRS "include" "vendor"
                    REQUIRES peer_event peer_frame espressif__esp-tflite-micro espressif__esp-dsp espressif__esp-nn)
