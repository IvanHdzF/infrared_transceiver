set(srcs "test_ble.c")


idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       # Add ESP_IDF libraries here as needed
                       REQUIRES nimble_ctrl unity bt
                       WHOLE_ARCHIVE
                    )
