set(srcs "stg_man.c")

idf_component_register(
    SRCS
        ${srcs}
    INCLUDE_DIRS
        "include"                # public headers
    PRIV_INCLUDE_DIRS
        "."                      # private headers
    REQUIRES
        retrofit_os
    PRIV_REQUIRES
        vfs
        fatfs
)
