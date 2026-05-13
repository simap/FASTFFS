/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS host inspection CLI: dump/check raw images and generate
 * deterministic host workload images for recovery testing.
 */

#include "fastffs/fastffs.h"
#include "fastffs/fastffs_host.h"
#include "fastffs/fastffs_inspect.h"
#include "fastffs/verify_flash.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
#define TOOL_INDEX_HASH_TABLE_SIZE FFFS_SLOT_COUNT
#else
#define TOOL_INDEX_HASH_TABLE_SIZE FFFS_INDEX_HASH_TABLE_SIZE
#endif

static uint16_t index_heads[TOOL_INDEX_HASH_TABLE_SIZE];

static int mount_tool_fs(struct fffs *fs, const struct fffs_backend *backend);

static void usage(FILE *out, const char *argv0) {
    fprintf(out, "usage:\n");
    fprintf(out, "  %s create <sector_size> <sector_count> <image> [index_sectors]\n",
            argv0);
    fprintf(out, "  %s dump <image>\n", argv0);
    fprintf(out, "  %s check <image>\n", argv0);
    fprintf(out, "  %s list <image>\n", argv0);
    fprintf(out, "  %s load <root> <image>\n", argv0);
    fprintf(out, "  %s workload <image> <bytes> [rounds]\n", argv0);
}

static int parse_size(const char *text, size_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || !end || *end || value == 0) {
        return FFFS_ERR_INVALID;
    }
    *out = (size_t)value;
    return FFFS_OK;
}

static int open_image(const char *path, struct ffsv_flash **flash,
        struct fffs_backend *backend) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return FFFS_ERR_IO;
    }
    int err = fseek(f, 0, SEEK_END);
    long len = err == 0 ? ftell(f) : -1;
    fclose(f);
    if (len <= 0) {
        return FFFS_ERR_INVALID;
    }

    err = ffsv_flash_create_with_preset(flash, FFSV_PRESET_TARGET_NOR_NOTES,
            (size_t)len);
    if (err != FFSV_OK) {
        return FFFS_ERR_INVALID;
    }
    err = ffsv_flash_load_image_file(*flash, path);
    if (err != FFSV_OK) {
        ffsv_flash_destroy(*flash);
        *flash = NULL;
        return FFFS_ERR_IO;
    }
    return fffs_host_backend_from_verify_flash(backend, *flash);
}

static void print_summary(const struct fffs_inspect_summary *s) {
    printf("sector_size=%zu sector_count=%zu index_sectors=%u "
            "active_index=%zu serial=%u\n",
            s->sector_size, s->sector_count, (unsigned)s->index_sectors,
            s->active_index_sector, (unsigned)s->active_index_serial);
    printf("index_records=%zu deletes=%zu index_corrupt=%zu "
            "live_entries=%zu live_corrupt=%zu\n",
            s->index_records, s->index_deletes, s->index_corrupt_records,
            s->live_entries, s->live_entries_corrupt);
    printf("data_erased=%zu data_owned=%zu data_tombstoned=%zu "
            "data_corrupt=%zu md_live=%zu md_obsolete_orphaned=%zu "
            "md_tombstoned=%zu md_corrupt=%zu\n",
            s->data_sectors_erased, s->data_sectors_owned,
            s->data_sectors_tombstoned, s->data_sectors_corrupt,
            s->md_live, s->md_obsolete_orphaned, s->md_tombstoned,
            s->md_corrupt);
}

static int cmd_dump(const char *path) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    int err = open_image(path, &flash, &backend);
    if (err == FFFS_OK) {
        err = fffs_inspect_dump(&backend, stdout);
    }
    ffsv_flash_destroy(flash);
    return err;
}

static int cmd_check(const char *path) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs_inspect_summary summary;
    int err = open_image(path, &flash, &backend);
    if (err == FFFS_OK) {
        err = fffs_inspect_check(&backend, &summary);
    }
    if (err == FFFS_OK) {
        print_summary(&summary);
        if (summary.index_corrupt_records || summary.live_entries_corrupt ||
                summary.data_sectors_corrupt || summary.md_corrupt) {
            err = FFFS_ERR_CORRUPT;
        }
    }
    ffsv_flash_destroy(flash);
    return err;
}

static int cmd_list(const char *path) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    bool mounted = false;
    int err = open_image(path, &flash, &backend);
    if (err == FFFS_OK) {
        err = mount_tool_fs(&fs, &backend);
        mounted = err == FFFS_OK;
    }

    size_t count = 0;
    size_t bytes = 0;
    if (err == FFFS_OK) {
        struct fffs_dir dir;
        struct fffs_stat stat;
        err = fffs_dir_open(&fs, &dir, NULL);
        while (err == FFFS_OK && fffs_dir_read(&dir, &stat)) {
            printf("%s\t%lu\n", stat.name, (unsigned long)stat.size);
            count++;
            bytes += stat.size;
        }
        if (err == FFFS_OK) {
            err = fffs_dir_status(&dir);
        }
        fffs_dir_close(&dir);
    }
    if (mounted) {
        fffs_unmount(&fs);
    }
    if (err == FFFS_OK) {
        fprintf(stderr, "listed %zu files %zu bytes\n", count, bytes);
    }
    ffsv_flash_destroy(flash);
    return err;
}

static int sector_enum_from_size(size_t sector_size,
        enum fffs_sector_size *out) {
    if (sector_size < ((size_t)256u << FFFS_MIN_SECTOR_SHIFT) ||
            sector_size > ((size_t)256u << FFFS_MAX_SECTOR_SHIFT)) {
        return FFFS_ERR_INVALID;
    }
    for (uint8_t shift = FFFS_MIN_SECTOR_SHIFT;
            shift <= FFFS_MAX_SECTOR_SHIFT; shift++) {
        if (((size_t)256u << shift) == sector_size) {
            *out = (enum fffs_sector_size)sector_size;
            return FFFS_OK;
        }
    }
    return FFFS_ERR_INVALID;
}

static int cmd_create(const char *sector_size_text,
        const char *sector_count_text, const char *path,
        const char *index_text) {
    size_t sector_size;
    int err = parse_size(sector_size_text, &sector_size);
    if (err != FFFS_OK) {
        return err;
    }
    size_t sector_count;
    err = parse_size(sector_count_text, &sector_count);
    if (err != FFFS_OK || sector_count > UINT16_MAX) {
        return FFFS_ERR_INVALID;
    }
    enum fffs_sector_size format_sector_size;
    err = sector_enum_from_size(sector_size, &format_sector_size);
    if (err != FFFS_OK) {
        return err;
    }

    size_t index_sectors = FFFS_DEFAULT_INDEX_SECTORS;
    if (index_text) {
        err = parse_size(index_text, &index_sectors);
        if (err != FFFS_OK || index_sectors > UINT8_MAX) {
            return FFFS_ERR_INVALID;
        }
    }
    if (sector_count > SIZE_MAX / sector_size) {
        return FFFS_ERR_INVALID;
    }
    size_t size = sector_size * sector_count;

    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    err = ffsv_flash_create_with_preset(&flash, FFSV_PRESET_TARGET_NOR_NOTES,
            size);
    if (err != FFSV_OK) {
        return FFFS_ERR_INVALID;
    }
    err = fffs_host_backend_from_verify_flash(&backend, flash);
    if (err == FFFS_OK) {
        err = fffs_format(&backend, &(struct fffs_format_options){
                    .index_sectors = (uint8_t)index_sectors,
                    .sector_size = format_sector_size,
                });
    }
    if (err == FFFS_OK) {
        err = ffsv_flash_dump_image(flash, path) == FFSV_OK ?
            FFFS_OK : FFFS_ERR_IO;
    }
    ffsv_flash_destroy(flash);
    return err;
}

static int mount_tool_fs(struct fffs *fs, const struct fffs_backend *backend) {
    return fffs_mount(fs, backend, &(struct fffs_mount_options){
        .index_heads = index_heads,
        .index_hash_table_size =
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
            FFFS_SLOT_COUNT,
#else
            FFFS_INDEX_HASH_TABLE_SIZE,
#endif
    });
}

static int cmd_workload(const char *path, const char *size_text,
        const char *rounds_text) {
    size_t size;
    int err = parse_size(size_text, &size);
    if (err != FFFS_OK) {
        return err;
    }
    size_t rounds = 100;
    if (rounds_text) {
        err = parse_size(rounds_text, &rounds);
        if (err != FFFS_OK) {
            return err;
        }
    }

    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    err = ffsv_flash_create_with_preset(&flash, FFSV_PRESET_TARGET_NOR_NOTES,
            size);
    if (err != FFSV_OK) {
        return FFFS_ERR_INVALID;
    }
    err = fffs_host_backend_from_verify_flash(&backend, flash);
    if (err == FFFS_OK) {
        err = fffs_format(&backend, NULL);
    }
    if (err == FFFS_OK) {
        err = mount_tool_fs(&fs, &backend);
    }
    if (err == FFFS_OK) {
        err = fffs_workload_run(&fs, &(struct fffs_workload_options){
                    .seed = 0x12345678u,
                    .rounds = rounds,
                    .file_count = 224,
                    .max_file_size = 350u * 1024u,
                }, NULL);
        fffs_unmount(&fs);
    }
    if (err == FFFS_OK) {
        err = ffsv_flash_dump_image(flash, path) == FFSV_OK ?
            FFFS_OK : FFFS_ERR_IO;
    }
    ffsv_flash_destroy(flash);
    return err;
}

struct load_plan {
    size_t files;
    size_t bytes;
};

static char *dup_trimmed_root(const char *root) {
    size_t len = strlen(root);
    while (len > 1 && root[len - 1] == '/') {
        len--;
    }
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, root, len);
    out[len] = '\0';
    return out;
}

static char *path_join(const char *a, const char *b) {
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    bool slash = alen > 0 && a[alen - 1] != '/';
    char *out = malloc(alen + (slash ? 1 : 0) + blen + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, a, alen);
    size_t pos = alen;
    if (slash) {
        out[pos++] = '/';
    }
    memcpy(out + pos, b, blen);
    out[pos + blen] = '\0';
    return out;
}

static const char *relative_name(const char *root, const char *path) {
    size_t root_len = strlen(root);
    const char *rel = path + root_len;
    if (*rel == '/') {
        rel++;
    }
    return rel;
}

static int scan_load_tree(const char *root, const char *path,
        struct load_plan *plan) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return FFFS_ERR_IO;
    }

    if (S_ISREG(st.st_mode)) {
        const char *name = relative_name(root, path);
        if (name[0] == '\0' || strlen(name) > FFFS_MAX_NAME) {
            return FFFS_ERR_INVALID;
        }
        plan->files += 1;
        plan->bytes += (size_t)st.st_size;
        return FFFS_OK;
    }
    if (!S_ISDIR(st.st_mode)) {
        return FFFS_OK;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return FFFS_ERR_IO;
    }
    int err = FFFS_OK;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char *child = path_join(path, ent->d_name);
        if (!child) {
            err = FFFS_ERR_NOMEM;
            break;
        }
        err = scan_load_tree(root, child, plan);
        free(child);
        if (err != FFFS_OK) {
            break;
        }
    }
    closedir(dir);
    return err;
}

static int load_file(struct fffs *fs, const char *host_path,
        const char *fs_name) {
    FILE *in = fopen(host_path, "rb");
    if (!in) {
        return FFFS_ERR_IO;
    }

    struct fffs_file file;
    int err = fffs_open(fs, &file, fs_name,
            FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC);
    uint8_t buffer[256];
    while (err == FFFS_OK) {
        size_t n = fread(buffer, 1, sizeof(buffer), in);
        if (n > 0) {
            size_t written = 0;
            err = fffs_write(&file, buffer, n, &written);
            if (err == FFFS_OK && written != n) {
                err = FFFS_ERR_IO;
            }
        }
        if (n < sizeof(buffer)) {
            if (ferror(in)) {
                err = FFFS_ERR_IO;
            }
            break;
        }
    }
    if (err == FFFS_OK) {
        err = fffs_close(&file);
    }
    fclose(in);
    return err;
}

static int load_tree(struct fffs *fs, const char *root, const char *path,
        size_t *loaded) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return FFFS_ERR_IO;
    }
    if (S_ISREG(st.st_mode)) {
        const char *name = relative_name(root, path);
        int err = load_file(fs, path, name);
        if (err == FFFS_OK) {
            *loaded += 1;
        }
        return err;
    }
    if (!S_ISDIR(st.st_mode)) {
        return FFFS_OK;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return FFFS_ERR_IO;
    }
    int err = FFFS_OK;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char *child = path_join(path, ent->d_name);
        if (!child) {
            err = FFFS_ERR_NOMEM;
            break;
        }
        err = load_tree(fs, root, child, loaded);
        free(child);
        if (err != FFFS_OK) {
            break;
        }
    }
    closedir(dir);
    return err;
}

static int cmd_load(const char *image, const char *root_arg) {
    char *root = dup_trimmed_root(root_arg);
    if (!root) {
        return FFFS_ERR_NOMEM;
    }

    int err = FFFS_OK;
    struct load_plan plan = {0};
    if (err == FFFS_OK) {
        err = scan_load_tree(root, root, &plan);
    }

    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    bool mounted = false;
    if (err == FFFS_OK) {
        err = open_image(image, &flash, &backend);
    }
    if (err == FFFS_OK) {
        err = mount_tool_fs(&fs, &backend);
        mounted = err == FFFS_OK;
    }

    size_t loaded = 0;
    if (err == FFFS_OK) {
        err = load_tree(&fs, root, root, &loaded);
    }
    if (mounted) {
        fffs_unmount(&fs);
    }
    if (err == FFFS_OK) {
        err = ffsv_flash_dump_image(flash, image) == FFSV_OK ?
            FFFS_OK : FFFS_ERR_IO;
    }
    if (err == FFFS_OK) {
        printf("loaded %zu files into %s\n", loaded, image);
    }

    ffsv_flash_destroy(flash);
    free(root);
    return err;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(stderr, argv[0]);
        return 2;
    }

    int err;
    if (strcmp(argv[1], "create") == 0 && (argc == 5 || argc == 6)) {
        err = cmd_create(argv[2], argv[3], argv[4],
                argc == 6 ? argv[5] : NULL);
    } else if (strcmp(argv[1], "dump") == 0 && argc == 3) {
        err = cmd_dump(argv[2]);
    } else if (strcmp(argv[1], "check") == 0 && argc == 3) {
        err = cmd_check(argv[2]);
    } else if (strcmp(argv[1], "list") == 0 && argc == 3) {
        err = cmd_list(argv[2]);
    } else if (strcmp(argv[1], "load") == 0 && argc == 4) {
        err = cmd_load(argv[3], argv[2]);
    } else if (strcmp(argv[1], "workload") == 0 &&
            (argc == 4 || argc == 5)) {
        err = cmd_workload(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
    } else {
        usage(stderr, argv[0]);
        return 2;
    }

    if (err != FFFS_OK) {
        fprintf(stderr, "fffs_tool: %s\n", fffs_status_name(err));
        return 1;
    }
    return 0;
}
