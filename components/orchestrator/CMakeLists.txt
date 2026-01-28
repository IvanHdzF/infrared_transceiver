set(srcs "orch.c")
if(CONFIG_ORCH_USE_MOCK_HANDLERS)
    list(APPEND srcs "orch_handlers_mock.c")
else()
    list(APPEND srcs "orch_handlers_prod.c")
endif()


idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "include"                # public headers
    PRIV_INCLUDE_DIRS
        "."                      # private headers (orch_handlers.h)
    REQUIRES
        retrofit_os
        evt_bus
)

