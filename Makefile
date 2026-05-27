CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
CPPFLAGS ?= -Iinclude
CPPFLAGS += -Itests/littlefs
CPPFLAGS += -Ibenchmarks/churn_model
BUILD_ROOT ?= build
BUILD_DIR ?= $(BUILD_ROOT)/default
SANITIZE_CFLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
PTHREAD_CFLAGS ?= -pthread
PTHREAD_LDLIBS ?= -pthread

DEPFLAGS = -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d)
COMPILE.c = $(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c
LINK.o = $(CC) $(CFLAGS) $(LDFLAGS)

CORE_SRCS = \
	src/fastffs.c \
	src/fffs_flash.c \
	src/fffs_index_log.c \
	src/fffs_sector.c \
	src/fffs_file_md.c \
	src/fffs_ram_index.c \
	src/fffs_hashtable_index.c \
	src/fffs_nocache_index.c \
	src/fffs_bitset.c \
	src/fffs_alloc.c \
	src/fffs_alloc_map.c \
	src/fffs_gc.c \
	src/fffs_inspect.c

HOST_SRCS = \
	src/fastffs_host.c \
	src/verify_flash.c

LITTLEFS_SRCS = \
	tests/littlefs/bd/lfs_emubd.c \
	tests/littlefs/lfs_util.c

CHURN_SRCS = \
	benchmarks/churn_model/churn_model.c

TEST_VERIFY_FLASH_SRCS = \
	tests/test_verify_flash.c

TEST_FASTFFS_SRCS = \
	tests/test_fastffs.c

FFFS_TOOL_SRCS = \
	tools/fffs_tool.c

FFFS_TIME_PROBE_SRCS = \
	tools/fffs_time_probe.c

FFFS_CHURN_PROBE_SRCS = \
	tools/fffs_churn_probe.c

FFFS_CRASH_SWEEP_SRCS = \
	tools/fffs_crash_sweep.c

FFFS_API_CRASH_SWEEP_SRCS = \
	tools/fffs_api_crash_sweep.c

CORE_OBJS = $(CORE_SRCS:%.c=$(BUILD_DIR)/%.o)
HOST_OBJS = $(HOST_SRCS:%.c=$(BUILD_DIR)/%.o)
LITTLEFS_OBJS = $(LITTLEFS_SRCS:%.c=$(BUILD_DIR)/%.o)
CHURN_OBJS = $(CHURN_SRCS:%.c=$(BUILD_DIR)/%.o)
TEST_VERIFY_FLASH_OBJS = $(TEST_VERIFY_FLASH_SRCS:%.c=$(BUILD_DIR)/%.o)
TEST_FASTFFS_OBJS = $(TEST_FASTFFS_SRCS:%.c=$(BUILD_DIR)/%.o)
FFFS_TOOL_OBJS = $(FFFS_TOOL_SRCS:%.c=$(BUILD_DIR)/%.o)
FFFS_TIME_PROBE_OBJS = $(FFFS_TIME_PROBE_SRCS:%.c=$(BUILD_DIR)/%.o)
FFFS_CHURN_PROBE_OBJS = $(FFFS_CHURN_PROBE_SRCS:%.c=$(BUILD_DIR)/%.o)
FFFS_CRASH_SWEEP_OBJS = $(FFFS_CRASH_SWEEP_SRCS:%.c=$(BUILD_DIR)/%.o)
FFFS_API_CRASH_SWEEP_OBJS = $(FFFS_API_CRASH_SWEEP_SRCS:%.c=$(BUILD_DIR)/%.o)

VERIFY_FLASH_LINK_OBJS = \
	$(BUILD_DIR)/src/verify_flash.o \
	$(LITTLEFS_OBJS)

FASTFFS_LINK_OBJS = \
	$(CORE_OBJS) \
	$(HOST_OBJS) \
	$(LITTLEFS_OBJS)

TEST_VERIFY_FLASH_LINK_OBJS = \
	$(VERIFY_FLASH_LINK_OBJS) \
	$(TEST_VERIFY_FLASH_OBJS)

TEST_FASTFFS_LINK_OBJS = \
	$(FASTFFS_LINK_OBJS) \
	$(TEST_FASTFFS_OBJS)

FFFS_TOOL_LINK_OBJS = \
	$(FASTFFS_LINK_OBJS) \
	$(FFFS_TOOL_OBJS)

FFFS_TIME_PROBE_LINK_OBJS = \
	$(FASTFFS_LINK_OBJS) \
	$(FFFS_TIME_PROBE_OBJS)

FFFS_CHURN_PROBE_LINK_OBJS = \
	$(FASTFFS_LINK_OBJS) \
	$(CHURN_OBJS) \
	$(FFFS_CHURN_PROBE_OBJS)

FFFS_CRASH_SWEEP_LINK_OBJS = \
	$(FASTFFS_LINK_OBJS) \
	$(FFFS_CRASH_SWEEP_OBJS)

FFFS_API_CRASH_SWEEP_LINK_OBJS = \
	$(FASTFFS_LINK_OBJS) \
	$(CHURN_OBJS) \
	$(FFFS_API_CRASH_SWEEP_OBJS)

ALL_OBJS = \
	$(CORE_OBJS) \
	$(HOST_OBJS) \
	$(LITTLEFS_OBJS) \
	$(CHURN_OBJS) \
	$(TEST_VERIFY_FLASH_OBJS) \
	$(TEST_FASTFFS_OBJS) \
	$(FFFS_TOOL_OBJS) \
	$(FFFS_TIME_PROBE_OBJS) \
	$(FFFS_CHURN_PROBE_OBJS) \
	$(FFFS_CRASH_SWEEP_OBJS) \
	$(FFFS_API_CRASH_SWEEP_OBJS)

DEPS = $(ALL_OBJS:.o=.d)

.DEFAULT_GOAL := all

.PHONY: all test test-timing test-timing-full-index test-timing-nocache \
	test-timing-nocache-small-scratch test-timing-compare test-churn \
	test-churn-full-index test-churn-nocache test-workload \
	test-crash-sweep test-api-crash-sweep test-sanitize test-full-index \
	test-nocache test-full-alloc-map clean

all: $(BUILD_DIR)/test_verify_flash $(BUILD_DIR)/test_fastffs \
	$(BUILD_DIR)/fffs_tool $(BUILD_DIR)/fffs_time_probe \
	$(BUILD_DIR)/fffs_churn_probe $(BUILD_DIR)/fffs_crash_sweep \
	$(BUILD_DIR)/fffs_api_crash_sweep

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(COMPILE.c) $< -o $@

$(BUILD_DIR)/tools/fffs_churn_probe.o: tools/fffs_churn_probe.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DFFFS_HOST_CHURN_IMAGE_PREFIX=\"$(BUILD_DIR)/churn\" \
		$(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/tools/fffs_api_crash_sweep.o: tools/fffs_api_crash_sweep.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PTHREAD_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/test_verify_flash: $(TEST_VERIFY_FLASH_LINK_OBJS)
	$(LINK.o) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/test_fastffs: $(TEST_FASTFFS_LINK_OBJS)
	$(LINK.o) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/fffs_tool: $(FFFS_TOOL_LINK_OBJS)
	$(LINK.o) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/fffs_time_probe: $(FFFS_TIME_PROBE_LINK_OBJS)
	$(LINK.o) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/fffs_churn_probe: $(FFFS_CHURN_PROBE_LINK_OBJS)
	$(LINK.o) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/fffs_crash_sweep: $(FFFS_CRASH_SWEEP_LINK_OBJS)
	$(LINK.o) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/fffs_api_crash_sweep: $(FFFS_API_CRASH_SWEEP_LINK_OBJS)
	$(LINK.o) $^ $(PTHREAD_LDLIBS) $(LDLIBS) -o $@

test: $(BUILD_DIR)/test_verify_flash $(BUILD_DIR)/test_fastffs \
		$(BUILD_DIR)/fffs_tool $(BUILD_DIR)/fffs_crash_sweep
	./$(BUILD_DIR)/test_verify_flash
	./$(BUILD_DIR)/test_fastffs
	./$(BUILD_DIR)/fffs_crash_sweep
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
	./$(BUILD_DIR)/fffs_api_crash_sweep -s 0x46464653 -n 1 -t 50 -w 2 -j 1

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

-include $(DEPS)
