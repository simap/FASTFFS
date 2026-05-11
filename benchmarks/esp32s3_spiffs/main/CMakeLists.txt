set(VFS_BENCH_COMMON "${CMAKE_CURRENT_LIST_DIR}/../../vfs_bench_common")

idf_component_register(
    SRCS
        "backend_spiffs.c"
        "${VFS_BENCH_COMMON}/vfs_bench.c"
    INCLUDE_DIRS
        "."
        "${VFS_BENCH_COMMON}"
    REQUIRES
        esp_partition
        esp_timer
        spiffs
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CHURN_TARGET_LIVE_PERCENT=60
    CHURN_TARGET_LIVE_BYTES=2308848
)
