set(srcs "test_ir_ctrl_main.c" "utilities.c")


idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       # Add ESP_IDF libraries here as needed
                       REQUIRES ir_ctrl esp_driver_rmt unity
                       WHOLE_ARCHIVE
                    )
