set(VFS_BENCH_COMMON "${CMAKE_CURRENT_LIST_DIR}/../../vfs_bench_common")
set(BENCHFS "${CMAKE_CURRENT_LIST_DIR}/../../benchfs")
set(CHURN_MODEL "${CMAKE_CURRENT_LIST_DIR}/../../churn_model")

idf_component_register(
    SRCS
        "backend_littlefs.c"
        "${VFS_BENCH_COMMON}/vfs_bench.c"
        "${BENCHFS}/benchfs.c"
        "${CHURN_MODEL}/churn_model.c"
    INCLUDE_DIRS
        "."
        "${VFS_BENCH_COMMON}"
        "${BENCHFS}"
        "${CHURN_MODEL}"
    REQUIRES
        esp_partition
        esp_timer
        littlefs
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CHURN_TARGET_LIVE_PERCENT=60
    CHURN_TARGET_LIVE_BYTES=2308848
)
