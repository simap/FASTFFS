CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
CPPFLAGS ?= -Iinclude
CPPFLAGS += -Itests/littlefs
BUILD_DIR ?= build
SANITIZE_CFLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test test-sanitize test-full-index clean

all: $(BUILD_DIR)/test_verify_flash $(BUILD_DIR)/test_fastffs

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

$(BUILD_DIR)/src_fffs_alloc.o: src/fffs_alloc.c src/fffs_internal.h \
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
		include/fastffs/fastffs_host.h include/fastffs/verify_flash.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_verify_flash: $(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/test_verify_flash.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test_fastffs: $(BUILD_DIR)/src_fastffs.o \
		$(BUILD_DIR)/src_fffs_io.o \
		$(BUILD_DIR)/src_fffs_ram_index.o \
		$(BUILD_DIR)/src_fffs_alloc.o \
		$(BUILD_DIR)/src_fastffs_host.o \
		$(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/test_fastffs.o
	$(CC) $(CFLAGS) $^ -o $@

test: $(BUILD_DIR)/test_verify_flash $(BUILD_DIR)/test_fastffs
	./$(BUILD_DIR)/test_verify_flash
	./$(BUILD_DIR)/test_fastffs

test-sanitize:
	$(MAKE) BUILD_DIR=build-sanitize CFLAGS="$(CFLAGS) $(SANITIZE_CFLAGS)" test

test-full-index:
	$(MAKE) BUILD_DIR=build-full-index \
		CPPFLAGS="$(CPPFLAGS) -DFFFS_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_FULL_SLOT_HEADS" \
		test

clean:
	rm -rf build
	rm -rf build-sanitize
	rm -rf build-full-index
