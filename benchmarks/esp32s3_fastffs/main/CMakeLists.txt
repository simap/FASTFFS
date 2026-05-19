set(FASTFFS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(BENCHFS "${CMAKE_CURRENT_LIST_DIR}/../../benchfs")
set(CHURN_MODEL "${CMAKE_CURRENT_LIST_DIR}/../../churn_model")

idf_component_register(
    SRCS
        "main.c"
        "${BENCHFS}/benchfs.c"
        "${BENCHFS}/benchfs_esp.c"
        "${CHURN_MODEL}/churn_model.c"
        "${FASTFFS_ROOT}/src/fastffs.c"
        "${FASTFFS_ROOT}/src/fffs_alloc.c"
        "${FASTFFS_ROOT}/src/fffs_alloc_map.c"
        "${FASTFFS_ROOT}/src/fffs_bitset.c"
        "${FASTFFS_ROOT}/src/fffs_gc.c"
        "${FASTFFS_ROOT}/src/fffs_hashtable_index.c"
        "${FASTFFS_ROOT}/src/fffs_io.c"
        "${FASTFFS_ROOT}/src/fffs_nocache_index.c"
        "${FASTFFS_ROOT}/src/fffs_ram_index.c"
    INCLUDE_DIRS
        "."
        "${BENCHFS}"
        "${CHURN_MODEL}"
        "${FASTFFS_ROOT}/include"
        "${FASTFFS_ROOT}/src"
    REQUIRES
        esp_partition
        esp_timer
)

set(FASTFFS_BENCH_INDEX_CACHE_MODE "FFFS_INDEX_CACHE_HASH_HEADS" CACHE STRING
    "FASTFFS index cache mode used by the ESP32-S3 benchmark")
set(FASTFFS_BENCH_INDEX_HASH_TABLE_SIZE "1024" CACHE STRING
    "FASTFFS hash-head table size used by the ESP32-S3 benchmark")
set(FASTFFS_BENCH_ALLOC_MAP_MODE "FFFS_ALLOC_MAP_FULL_BITMAP" CACHE STRING
    "FASTFFS allocation map mode used by the ESP32-S3 benchmark")
set(FASTFFS_BENCH_CHURN_GC_POLICY "FASTFFS_CHURN_GC_POLICY_NONE" CACHE STRING
    "FASTFFS benchmark-side churn GC policy")
set(FASTFFS_BENCH_SCRATCH_SIZE "512" CACHE STRING
    "FASTFFS scratch buffer size used by the ESP32-S3 benchmark")
set(FASTFFS_BENCH_FILE_WRITE_BUFFER "256" CACHE STRING
    "FASTFFS per-open file write buffer size used by the ESP32-S3 benchmark")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FFFS_INDEX_CACHE_MODE=${FASTFFS_BENCH_INDEX_CACHE_MODE}
    FFFS_INDEX_HASH_TABLE_SIZE=${FASTFFS_BENCH_INDEX_HASH_TABLE_SIZE}
    FFFS_ALLOC_MAP_MODE=${FASTFFS_BENCH_ALLOC_MAP_MODE}
    FFFS_FILE_WRITE_BUFFER=${FASTFFS_BENCH_FILE_WRITE_BUFFER}
    FASTFFS_CHURN_GC_POLICY=${FASTFFS_BENCH_CHURN_GC_POLICY}
    FASTFFS_SCRATCH_SIZE=${FASTFFS_BENCH_SCRATCH_SIZE}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-format
    -Wno-unused-function
    -Wno-type-limits
)
