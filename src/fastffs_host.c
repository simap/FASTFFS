#include "fastffs/fastffs_host.h"

static int host_read(void *ctx, size_t offset, void *buffer, size_t size) {
    return ffsv_flash_read(ctx, offset, buffer, size, FFSV_CALLSITE);
}

static int host_program(void *ctx, size_t offset,
        const void *buffer, size_t size) {
    return ffsv_flash_program(ctx, offset, buffer, size, FFSV_CALLSITE);
}

static int host_erase(void *ctx, size_t offset, size_t size) {
    return ffsv_flash_erase(ctx, offset, size, FFSV_CALLSITE);
}

int fffs_host_backend_from_verify_flash(struct fffs_backend *backend,
        struct ffsv_flash *flash) {
    if (!backend || !flash) {
        return FFFS_ERR_INVALID;
    }
    *backend = (struct fffs_backend){
        .ctx = flash,
        .size = ffsv_flash_size(flash),
        .read_granule = 1,
        .program_granule = 4,
        .read = host_read,
        .program = host_program,
        .erase = host_erase,
    };
    return FFFS_OK;
}
