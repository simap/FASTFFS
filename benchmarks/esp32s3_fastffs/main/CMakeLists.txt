set(FASTFFS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(CHURN_MODEL "${CMAKE_CURRENT_LIST_DIR}/../../churn_model")

idf_component_register(
    SRCS
        "main.c"
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
        "${CHURN_MODEL}"
        "${FASTFFS_ROOT}/include"
        "${FASTFFS_ROOT}/src"
    REQUIRES
        esp_partition
        esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-format
    -Wno-unused-function
    -Wno-type-limits
)
