set(srcs "test_orchestrator_main.c")


idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       # Add ESP_IDF libraries here as needed
                       REQUIRES orchestrator unity
                       WHOLE_ARCHIVE
                    )
