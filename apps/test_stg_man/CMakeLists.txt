set(srcs "test_stg_man_main.c")


idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       # Add ESP_IDF libraries here as needed
                       REQUIRES stg_man unity
                       WHOLE_ARCHIVE
                    )
