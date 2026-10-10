idf_component_register(SRCS "peer_event_map.c" "peer_judge.c" "peer_event.c"
                    INCLUDE_DIRS "include"
                    REQUIRES peer_frame log esp_common freertos)
