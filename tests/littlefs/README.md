# Imported littlefs files

This directory contains a small reference import from upstream littlefs:

- Repository: https://github.com/littlefs-project/littlefs
- Release tag: `v2.11.2`
- License: BSD-3-Clause, preserved in `LICENSE.md`
- Import purpose: seed/reference code for the FASTFFS host verification
  framework, especially the emulated block-device testing hooks.

Copied files:

- `lfs.h`
- `lfs_util.h`
- `lfs_util.c`
- `bd/lfs_emubd.h`
- `bd/lfs_emubd.c`
- `bd/lfs_filebd.h`
- `bd/lfs_filebd.c`
- `bd/lfs_rambd.h`
- `bd/lfs_rambd.c`

These files should remain close to the upstream release snapshot. FASTFFS-specific
NOR flash semantics, staged writes, crash injection, operation tracing, and fake
timing should live beside these rather than being mixed into the reference
copy where possible.
