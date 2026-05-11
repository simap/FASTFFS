CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
CPPFLAGS ?= -Iinclude
CPPFLAGS += -Itests/littlefs
BUILD_DIR ?= build
SANITIZE_CFLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test test-sanitize clean

all: $(BUILD_DIR)/test_verify_flash

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/tests/littlefs/bd
	mkdir -p $(BUILD_DIR)/tests/littlefs

$(BUILD_DIR)/src_verify_flash.o: src/verify_flash.c include/fastffs/verify_flash.h \
		tests/littlefs/bd/lfs_emubd.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o: \
		tests/littlefs/bd/lfs_emubd.c \
		tests/littlefs/bd/lfs_emubd.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/littlefs/lfs_util.o: tests/littlefs/lfs_util.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_verify_flash.o: tests/test_verify_flash.c include/fastffs/verify_flash.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_verify_flash: $(BUILD_DIR)/src_verify_flash.o \
		$(BUILD_DIR)/tests/littlefs/bd/lfs_emubd.o \
		$(BUILD_DIR)/tests/littlefs/lfs_util.o \
		$(BUILD_DIR)/test_verify_flash.o
	$(CC) $(CFLAGS) $^ -o $@

test: $(BUILD_DIR)/test_verify_flash
	./$(BUILD_DIR)/test_verify_flash

test-sanitize:
	$(MAKE) BUILD_DIR=build-sanitize CFLAGS="$(CFLAGS) $(SANITIZE_CFLAGS)" test

clean:
	rm -rf build
	rm -rf build-sanitize
