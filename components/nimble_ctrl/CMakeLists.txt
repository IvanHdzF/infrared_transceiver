set(
    srcs
    "nimble_ctrl.c"
    "nimble_ctrl_gap.c"
    "nimble_ctrl_gatt.c"
)

idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "include"                # public headers
    PRIV_INCLUDE_DIRS
        "."                      # private headers
    REQUIRES

    PRIV_REQUIRES
        bt
        nvs_flash
)
