#ifndef FASTFFS_FASTFFS_HOST_H
#define FASTFFS_FASTFFS_HOST_H

#include "fastffs/fastffs.h"
#include "fastffs/verify_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

int fffs_host_backend_from_verify_flash(struct fffs_backend *backend,
        struct ffsv_flash *flash);

#ifdef __cplusplus
}
#endif

#endif
