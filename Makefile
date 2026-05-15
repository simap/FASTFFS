CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
CPPFLAGS ?= -Iinclude
CPPFLAGS += -Itests/littlefs
CPPFLAGS += -Ibenchmarks/churn_model
BUILD_ROOT ?= build
BUILD_DIR ?= $(BUILD_ROOT)/default
SANITIZE_CFLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
PTHREAD_FLAGS ?= -pthread

.PHONY: all test test-timing test-timing-full-index test-timing-nocache \
	test-timing-nocache-small-scratch test-timing-compare test-churn \
	test-churn-full-index test-churn-nocache test-workload \
	test-crash-sweep test-api-crash-sweep test-sanitize test-full-index \
	test-nocache test-full-alloc-map clean

all: $(BUILD_DIR)/test_verify_flash $(BUILD_DIR)/test_fastffs \
	$(BUILD_DIR)/fffs_tool $(BUILD_DIR)/fffs_time_probe \
	$(BUILD_DIR)/fffs_churn_probe $(BUILD_DIR)/fffs_crash_sweep \
	$(BUILD_DIR)/fffs_api_crash_sweep

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/benchmarks
	mkdir -p $(BUILD_DIR)/tests/littlefs/bd
	mkdir -p $(BUILD_DIR)/tests/littlefs

$(BUILD_DIR)/src_verify_flash.o: src/verify_flash.c include/fastffs/verify_flash.h \
		tests/littlefs/bd/lfs_emubd.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fastffs.o: src/fastffs.c include/fastffs/fastffs.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fffs_io.o: src/fffs_io.c src/fffs_internal.h \
		include/fastffs/fastffs.h include/fastffs/fffs_opts.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fffs_ram_index.o: src/fffs_ram_index.c \
		src/fffs_internal.h include/fastffs/fastffs.h \
		include/fastffs/fffs_opts.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fffs_hashtable_index.o: src/fffs_hashtable_index.c \
		src/fffs_internal.h include/fastffs/fastffs.h \
		include/fastffs/fffs_opts.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fffs_nocache_index.o: src/fffs_nocache_index.c \
		src/fffs_internal.h include/fastffs/fastffs.h \
		include/fastffs/fffs_opts.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fffs_bitset.o: src/fffs_bitset.c \
		src/fffs_internal.h include/fastffs/fastffs.h \
		include/fastffs/fffs_opts.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fffs_alloc.o: src/fffs_alloc.c src/fffs_internal.h \
		include/fastffs/fastffs.h include/fastffs/fffs_opts.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fffs_alloc_map.o: src/fffs_alloc_map.c src/fffs_internal.h \
		include/fastffs/fastffs.h include/fastffs/fffs_opts.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fffs_gc.o: src/fffs_gc.c src/fffs_internal.h \
		include/fastffs/fastffs.h include/fastffs/fffs_opts.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fffs_inspect.o: src/fffs_inspect.c \
		src/fffs_internal.h include/fastffs/fastffs_inspect.h \
		include/fastffs/fastffs.h include/fastffs/fffs_opts.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src_fastffs_host.o: src/fastffs_host.c \
		include/fastffs/fastffs_host.h include/fastffs/fastffs.h \
		include/fastffs/verify_flash.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/benchmarks/churn_model.o: benchmarks/churn_model/churn_model.c \
		benchmarks/churn_model/churn_model.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o: \
		tests/littlefs/bd/lfs_emubd.c \
		tests/littlefs/bd/lfs_emubd.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/littlefs/lfs_util.o: tests/littlefs/lfs_util.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_verify_flash.o: tests/test_verify_flash.c include/fastffs/verify_flash.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_fastffs.o: tests/test_fastffs.c include/fastffs/fastffs.h \
		include/fastffs/fastffs_host.h include/fastffs/fastffs_inspect.h \
		include/fastffs/verify_flash.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fffs_tool.o: tools/fffs_tool.c include/fastffs/fastffs.h \
		include/fastffs/fastffs_host.h include/fastffs/fastffs_inspect.h \
		include/fastffs/verify_flash.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fffs_time_probe.o: tools/fffs_time_probe.c \
		include/fastffs/fastffs.h include/fastffs/fastffs_host.h \
		include/fastffs/verify_flash.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fffs_churn_probe.o: tools/fffs_churn_probe.c \
		include/fastffs/fastffs.h include/fastffs/fastffs_host.h \
		include/fastffs/verify_flash.h benchmarks/churn_model/churn_model.h \
		| $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -DFFFS_HOST_CHURN_IMAGE_PREFIX=\"$(BUILD_DIR)/churn\" \
		$(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fffs_crash_sweep.o: tools/fffs_crash_sweep.c \
		include/fastffs/fastffs.h include/fastffs/fastffs_host.h \
		include/fastffs/fastffs_inspect.h include/fastffs/verify_flash.h \
		| $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fffs_api_crash_sweep.o: tools/fffs_api_crash_sweep.c \
		include/fastffs/fastffs.h include/fastffs/fastffs_host.h \
		include/fastffs/fastffs_inspect.h include/fastffs/verify_flash.h \
		benchmarks/churn_model/churn_model.h src/fffs_internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PTHREAD_FLAGS) -c $< -o $@

$(BUILD_DIR)/test_verify_flash: $(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/test_verify_flash.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test_fastffs: $(BUILD_DIR)/src_fastffs.o \
		$(BUILD_DIR)/src_fffs_io.o \
		$(BUILD_DIR)/src_fffs_ram_index.o \
		$(BUILD_DIR)/src_fffs_hashtable_index.o \
		$(BUILD_DIR)/src_fffs_nocache_index.o \
		$(BUILD_DIR)/src_fffs_bitset.o \
		$(BUILD_DIR)/src_fffs_alloc.o \
		$(BUILD_DIR)/src_fffs_alloc_map.o \
		$(BUILD_DIR)/src_fffs_gc.o \
		$(BUILD_DIR)/src_fffs_inspect.o \
		$(BUILD_DIR)/src_fastffs_host.o \
		$(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/test_fastffs.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/fffs_tool: $(BUILD_DIR)/src_fastffs.o \
		$(BUILD_DIR)/src_fffs_io.o \
		$(BUILD_DIR)/src_fffs_ram_index.o \
		$(BUILD_DIR)/src_fffs_hashtable_index.o \
		$(BUILD_DIR)/src_fffs_nocache_index.o \
		$(BUILD_DIR)/src_fffs_bitset.o \
		$(BUILD_DIR)/src_fffs_alloc.o \
		$(BUILD_DIR)/src_fffs_alloc_map.o \
		$(BUILD_DIR)/src_fffs_gc.o \
		$(BUILD_DIR)/src_fffs_inspect.o \
		$(BUILD_DIR)/src_fastffs_host.o \
		$(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/fffs_tool.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/fffs_time_probe: $(BUILD_DIR)/src_fastffs.o \
		$(BUILD_DIR)/src_fffs_io.o \
		$(BUILD_DIR)/src_fffs_ram_index.o \
		$(BUILD_DIR)/src_fffs_hashtable_index.o \
		$(BUILD_DIR)/src_fffs_nocache_index.o \
		$(BUILD_DIR)/src_fffs_bitset.o \
		$(BUILD_DIR)/src_fffs_alloc.o \
		$(BUILD_DIR)/src_fffs_alloc_map.o \
		$(BUILD_DIR)/src_fffs_gc.o \
		$(BUILD_DIR)/src_fffs_inspect.o \
		$(BUILD_DIR)/src_fastffs_host.o \
		$(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/fffs_time_probe.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/fffs_churn_probe: $(BUILD_DIR)/src_fastffs.o \
		$(BUILD_DIR)/src_fffs_io.o \
		$(BUILD_DIR)/src_fffs_ram_index.o \
		$(BUILD_DIR)/src_fffs_hashtable_index.o \
		$(BUILD_DIR)/src_fffs_nocache_index.o \
		$(BUILD_DIR)/src_fffs_bitset.o \
		$(BUILD_DIR)/src_fffs_alloc.o \
		$(BUILD_DIR)/src_fffs_alloc_map.o \
		$(BUILD_DIR)/src_fffs_gc.o \
		$(BUILD_DIR)/src_fffs_inspect.o \
		$(BUILD_DIR)/src_fastffs_host.o \
		$(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/benchmarks/churn_model.o \
		$(BUILD_DIR)/fffs_churn_probe.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/fffs_crash_sweep: $(BUILD_DIR)/src_fastffs.o \
		$(BUILD_DIR)/src_fffs_io.o \
		$(BUILD_DIR)/src_fffs_ram_index.o \
		$(BUILD_DIR)/src_fffs_hashtable_index.o \
		$(BUILD_DIR)/src_fffs_nocache_index.o \
		$(BUILD_DIR)/src_fffs_bitset.o \
		$(BUILD_DIR)/src_fffs_alloc.o \
		$(BUILD_DIR)/src_fffs_alloc_map.o \
		$(BUILD_DIR)/src_fffs_gc.o \
		$(BUILD_DIR)/src_fffs_inspect.o \
		$(BUILD_DIR)/src_fastffs_host.o \
		$(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/fffs_crash_sweep.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/fffs_api_crash_sweep: $(BUILD_DIR)/src_fastffs.o \
		$(BUILD_DIR)/src_fffs_io.o \
		$(BUILD_DIR)/src_fffs_ram_index.o \
		$(BUILD_DIR)/src_fffs_hashtable_index.o \
		$(BUILD_DIR)/src_fffs_nocache_index.o \
		$(BUILD_DIR)/src_fffs_bitset.o \
		$(BUILD_DIR)/src_fffs_alloc.o \
		$(BUILD_DIR)/src_fffs_alloc_map.o \
		$(BUILD_DIR)/src_fffs_gc.o \
		$(BUILD_DIR)/src_fffs_inspect.o \
		$(BUILD_DIR)/src_fastffs_host.o \
		$(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/benchmarks/churn_model.o \
		$(BUILD_DIR)/fffs_api_crash_sweep.o
	$(CC) $(CFLAGS) $(PTHREAD_FLAGS) $^ -o $@

test: $(BUILD_DIR)/test_verify_flash $(BUILD_DIR)/test_fastffs \
		$(BUILD_DIR)/fffs_tool
	./$(BUILD_DIR)/test_verify_flash
	./$(BUILD_DIR)/test_fastffs
	./$(BUILD_DIR)/fffs_tool workload $(BUILD_DIR)/workload.img 524288 4
	./$(BUILD_DIR)/fffs_tool check $(BUILD_DIR)/workload.img
	./$(BUILD_DIR)/fffs_tool create 4096 32 $(BUILD_DIR)/load.img
	./$(BUILD_DIR)/fffs_tool load tests/fixtures/load_root $(BUILD_DIR)/load.img
	./$(BUILD_DIR)/fffs_tool check $(BUILD_DIR)/load.img

test-timing: $(BUILD_DIR)/fffs_time_probe
	./$(BUILD_DIR)/fffs_time_probe

test-timing-full-index:
	$(MAKE) BUILD_DIR=$(BUILD_ROOT)/full-index \
		CPPFLAGS="$(CPPFLAGS) -DFFFS_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_FULL_SLOT_HEADS" \
		test-timing

test-timing-nocache:
	$(MAKE) BUILD_DIR=$(BUILD_ROOT)/nocache \
		CPPFLAGS="$(CPPFLAGS) -DFFFS_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_NONE" \
		test-timing

test-timing-nocache-small-scratch:
	$(MAKE) BUILD_DIR=$(BUILD_ROOT)/nocache-small-scratch \
		CPPFLAGS="$(CPPFLAGS) -DFFFS_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_NONE -DFFFS_TIME_PROBE_SCRATCH_SIZE=64" \
		test-timing

test-timing-compare: test-timing test-timing-full-index test-timing-nocache

test-churn: $(BUILD_DIR)/fffs_churn_probe
	./$(BUILD_DIR)/fffs_churn_probe

test-churn-full-index:
	$(MAKE) BUILD_DIR=$(BUILD_ROOT)/churn-full-index \
		CPPFLAGS="$(CPPFLAGS) -DFFFS_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_FULL_SLOT_HEADS" \
		test-churn

test-churn-nocache:
	$(MAKE) BUILD_DIR=$(BUILD_ROOT)/churn-nocache \
		CPPFLAGS="$(CPPFLAGS) -DFFFS_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_NONE" \
		test-churn

test-workload: $(BUILD_DIR)/fffs_tool
	./$(BUILD_DIR)/fffs_tool workload $(BUILD_DIR)/workload-long.img 4194304 16
	./$(BUILD_DIR)/fffs_tool check $(BUILD_DIR)/workload-long.img

test-crash-sweep: $(BUILD_DIR)/fffs_crash_sweep
	./$(BUILD_DIR)/fffs_crash_sweep

test-api-crash-sweep: $(BUILD_DIR)/fffs_api_crash_sweep
	./$(BUILD_DIR)/fffs_api_crash_sweep 0xa11ce000 8 10000 256

test-sanitize:
	$(MAKE) BUILD_DIR=$(BUILD_ROOT)/sanitize CFLAGS="$(CFLAGS) $(SANITIZE_CFLAGS)" test

test-full-index:
	$(MAKE) BUILD_DIR=$(BUILD_ROOT)/full-index \
		CPPFLAGS="$(CPPFLAGS) -DFFFS_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_FULL_SLOT_HEADS" \
		test

test-nocache:
	$(MAKE) BUILD_DIR=$(BUILD_ROOT)/nocache \
		CPPFLAGS="$(CPPFLAGS) -DFFFS_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_NONE" \
		test

test-full-alloc-map:
	$(MAKE) BUILD_DIR=$(BUILD_ROOT)/full-alloc-map \
		CPPFLAGS="$(CPPFLAGS) -DFFFS_ALLOC_MAP_MODE=FFFS_ALLOC_MAP_FULL_BITMAP" \
		test

clean:
	rm -rf $(BUILD_ROOT)
