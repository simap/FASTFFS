set(JESFS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/JesFs")

idf_component_register(
    SRCS
        "main.c"
        "jesfs_ll_esp_partition.c"
        "jesfs_platform_hooks.c"
        "${JESFS_ROOT}/jesfs_hl.c"
        "${JESFS_ROOT}/jesfs_ml.c"
    INCLUDE_DIRS
        "."
        "${JESFS_ROOT}"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MACRONIX_MANU_TYP=MACRONIX_MANU_TYP_RX
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-format
    -Wno-unused-function
    -Wno-error=implicit-fallthrough
)
