CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
CPPFLAGS ?= -Iinclude
CPPFLAGS += -Itests/littlefs
BUILD_ROOT ?= build
BUILD_DIR ?= $(BUILD_ROOT)/default
SANITIZE_CFLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test test-timing test-timing-full-index test-timing-nocache \
	test-timing-nocache-noscratch test-timing-compare test-workload \
	test-sanitize test-full-index test-nocache clean

all: $(BUILD_DIR)/test_verify_flash $(BUILD_DIR)/test_fastffs \
	$(BUILD_DIR)/fffs_tool $(BUILD_DIR)/fffs_time_probe

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
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
		$(BUILD_DIR)/src_fffs_gc.o \
		$(BUILD_DIR)/src_fffs_inspect.o \
		$(BUILD_DIR)/src_fastffs_host.o \
		$(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/fffs_time_probe.o
	$(CC) $(CFLAGS) $^ -o $@

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

test-timing-nocache-noscratch:
	$(MAKE) BUILD_DIR=$(BUILD_ROOT)/nocache-noscratch \
		CPPFLAGS="$(CPPFLAGS) -DFFFS_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_NONE -DFFFS_TIME_PROBE_SCRATCH_SIZE=0" \
		test-timing

test-timing-compare: test-timing test-timing-full-index test-timing-nocache

test-workload: $(BUILD_DIR)/fffs_tool
	./$(BUILD_DIR)/fffs_tool workload $(BUILD_DIR)/workload-long.img 4194304 16
	./$(BUILD_DIR)/fffs_tool check $(BUILD_DIR)/workload-long.img

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

clean:
	rm -rf $(BUILD_ROOT)
