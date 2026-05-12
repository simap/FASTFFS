#ifndef FASTFFS_FASTFFS_H
#define FASTFFS_FASTFFS_H

#include "fastffs/fffs_opts.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum fffs_status {
    FFFS_OK = 0,
    FFFS_ERR_INVALID = -1,
    FFFS_ERR_NOMEM = -2,
    FFFS_ERR_RANGE = -3,
    FFFS_ERR_NO_SPACE = -4,
    FFFS_ERR_NOT_FOUND = -5,
    FFFS_ERR_EXISTS = -6,
    FFFS_ERR_NAME_TOO_LONG = -7,
    FFFS_ERR_CORRUPT = -8,
    FFFS_ERR_IO = -9,
};

enum fffs_open_flags {
    FFFS_O_RDONLY = 0x01,
    FFFS_O_WRONLY = 0x02,
    FFFS_O_CREATE = 0x04,
    FFFS_O_TRUNC = 0x08,
    FFFS_O_EXCL = 0x10,
};

enum {
    FFFS_DEFAULT_SECTOR_SIZE = 4096,
    FFFS_DEFAULT_SECTOR_SHIFT = 4,
    FFFS_DEFAULT_INDEX_SECTORS = 2,
    FFFS_MAX_NAME = 32,
    FFFS_MAX_PROBE_DISTANCE = 50,
    FFFS_MAX_PROGRAM_GRANULE = 256,
    FFFS_SLOT_COUNT = 65536,
};

struct fffs_backend {
    void *ctx;
    size_t size;
    size_t read_granule;
    size_t program_granule;
    int (*read)(void *ctx, size_t offset, void *buffer, size_t size);
    int (*program)(void *ctx, size_t offset, const void *buffer, size_t size);
    int (*erase)(void *ctx, size_t offset, size_t size);
};

struct fffs_format_options {
    uint8_t index_sectors;
    uint8_t sector_shift;
};

struct fffs_mount_options {
    uint16_t *index_heads;
    size_t index_head_count;
};

struct fffs {
    struct fffs_backend backend;
    uint16_t *index_heads;
    size_t index_head_count;
    size_t sector_size;
    size_t sector_count;
    uint8_t sector_shift;
    uint8_t index_sectors;
    size_t active_index_sector;
    size_t next_index_offset;
};

struct fffs_stat {
    char name[FFFS_MAX_NAME + 1];
    uint32_t size;
};

typedef struct fffs_stat fffs_dirent;

struct fffs_file {
    struct fffs *fs;
    uint32_t flags;
    uint16_t slot;
    uint16_t head;
    uint16_t data_offset;
    uint32_t size;
    uint32_t pos;
    size_t tail_len;
    char name[FFFS_MAX_NAME + 1];
    uint8_t tail[FFFS_MAX_PROGRAM_GRANULE];
    bool found;
    bool closed;
};

struct fffs_dir {
    struct fffs *fs;
    size_t pos;
    size_t prefix_len;
    int status;
    char prefix[FFFS_MAX_NAME + 1];
    bool closed;
};

int fffs_format(const struct fffs_backend *backend,
        const struct fffs_format_options *options);
int fffs_mount(struct fffs *fs, const struct fffs_backend *backend,
        const struct fffs_mount_options *options);
void fffs_unmount(struct fffs *fs);

int fffs_open(struct fffs *fs, struct fffs_file *file,
        const char *name, uint32_t flags);
int fffs_close(struct fffs_file *file);
int fffs_read(struct fffs_file *file, void *buffer, size_t size,
        size_t *out_read);
int fffs_write(struct fffs_file *file, const void *buffer, size_t size,
        size_t *out_written);
int fffs_fstat(struct fffs_file *file, struct fffs_stat *st);

int fffs_stat(struct fffs *fs, const char *name, struct fffs_stat *st);
int fffs_exists(struct fffs *fs, const char *name, bool *exists);
int fffs_delete_file(struct fffs *fs, const char *name);
int fffs_dir_open(struct fffs *fs, struct fffs_dir *dir,
        const char *prefix);
bool fffs_dir_read(struct fffs_dir *dir, struct fffs_stat *st);
int fffs_dir_status(const struct fffs_dir *dir);
int fffs_dir_close(struct fffs_dir *dir);
int fffs_list(struct fffs *fs, struct fffs_stat *entries,
        size_t capacity, size_t *out_count);

const char *fffs_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif
