idf_component_register(SRCS "test_peer_model_window.c" "peer_model_window.c"
                         "peer_model.cc"
                         "vendor/hamic_model.c"
                         "vendor/hamic_inference.c"
                         "vendor/hamic_frontend.c"
                    INCLUDE_DIRS "include" "vendor"
                    REQUIRES peer_event peer_frame es7243e_mic freertos log heap)
set_source_files_properties(vendor/hamic_model.c vendor/hamic_inference.c vendor/hamic_frontend.c
                            PROPERTIES COMPILE_OPTIONS "-O2;-ffp-contract=off")
