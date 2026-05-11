CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
CPPFLAGS ?= -Iinclude
CPPFLAGS += -Itests/littlefs

.PHONY: all test clean

all: build/test_verify_flash

build:
	mkdir -p build
	mkdir -p build/tests/littlefs/bd
	mkdir -p build/tests/littlefs

build/src_verify_flash.o: src/verify_flash.c include/fastffs/verify_flash.h \
		tests/littlefs/bd/lfs_emubd.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/tests/littlefs/bd/lfs_emubd.o: \
		tests/littlefs/bd/lfs_emubd.c \
		tests/littlefs/bd/lfs_emubd.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/tests/littlefs/lfs_util.o: tests/littlefs/lfs_util.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/test_verify_flash.o: tests/test_verify_flash.c include/fastffs/verify_flash.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/test_verify_flash: build/src_verify_flash.o \
		build/tests/littlefs/bd/lfs_emubd.o \
		build/tests/littlefs/lfs_util.o \
		build/test_verify_flash.o
	$(CC) $(CFLAGS) $^ -o $@

test: build/test_verify_flash
	./build/test_verify_flash

clean:
	rm -rf build
