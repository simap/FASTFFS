# Installing and Bring-Up

FASTFFS is source-integrated. Copy the src/ and include/ code, but leave these out for MCU:

```
src/fastffs_host.c
src/fffs_inspect.c
src/verify_flash.c
include/fastffs/fastffs_host.h
include/fastffs/fastffs_inspect.h
include/fastffs/verify_flash.h
```

## Configuration

Set defines in your build system, or edit fffs_opts.h. The defaults are reasonable; the critical ones:

- `FFFS_INDEX_CACHE_MODE` picks the RAM namespace cache and changes what `fffs_mount()` needs. `FFFS_INDEX_CACHE_HASH_HEADS` (default): caller provides a power-of-two hash table, 4 bytes per bucket. `FFFS_INDEX_CACHE_NONE`: no `index_cache` needed at all (NULL is fine); lookups, listing, and GC liveness scan the on-flash index instead, so it is the smallest RAM and the slowest namespace. `FFFS_INDEX_CACHE_FULL_SLOT_HEADS`: direct slot lookup, needs `index_hash_table_size = 65536` and 128 KiB of `index_cache`.
- `FFFS_ALLOC_MAP_MODE`: `FFFS_ALLOC_MAP_FULL_BITMAP` (default) keeps a 1-bit-per-sector allocation hint map. `FFFS_ALLOC_MAP_NONE` compiles the `alloc_map`/`alloc_map_words` mount fields out entirely and trades that RAM for more flash scanning.
- `FFFS_FILE_CACHE_SIZE` (default 256): the read/write cache built into every `struct fffs_file`, so this is RAM per open file. Must be at least `program_granule`.
- `FFFS_GC_ON_ALLOC_FAILURE` (default 1): when allocation finds no erased sector, GC and compaction run inline, so a write on a full-but-reclaimable filesystem stalls instead of failing. Set to 0 for bounded write latency: such writes return `FFFS_ERR_NO_SPACE` immediately, and freed space becomes usable only after you run GC steps yourself.
- `FFFS_LAZY_DELETE_TOMBSTONES` (default 0): deletes and overwrites eagerly tombstone the replaced chain. Set to 1 to defer dead-page discovery to GC: faster deletes, more GC work later.

## Backend

This is the backend driver you'll provide to talk with the flash. FASTFFS will start at offset 0, so you can offset ranges if you want to give it a specific partition in your backend.

Your driver should return `0` for success, nonzero for failure.

```c
#include "fastffs/fastffs.h"

static int flash_read(void *ctx, size_t off, void *buf, size_t len);
static int flash_program(void *ctx, size_t off, const void *buf, size_t len);
static int flash_erase(void *ctx, size_t off, size_t len);

static struct fffs_backend backend = {
    .ctx = (void *) 0, //or pass some data you want to give to your driver
    .size = 2097152, //the flash/partition size
    .program_granule = 1, //if your flash requires a minimum size/alignment for programming
    .read = flash_read,
    .program = flash_program,
    .erase = flash_erase,
};
```

Backend driver contract:

- All three callbacks are synchronous and blocking: the operation must be fully complete on flash when the callback returns. FASTFFS immediately reuses source buffers and reads back what it just wrote, so queued or lazy writeback and fire-and-forget DMA will corrupt data. DMA is fine only if the adapter waits for completion before returning. Never report success for queued work.
- `read()` copies exactly `len` bytes to `buf` from `off`. FASTFFS requires byte-readable access: reads can be any size or offset, with no alignment guarantees. If the raw flash driver has stricter read alignment, hide that inside the adapter with a temporary aligned read and copy-out. FASTFFS will eagerly read larger areas when it might scan nearby, and tiny reads when it doesn't need more.
- `program()` programs exactly `len` bytes from `buf` to `off` and never erases. FASTFFS aligns calls to `program_granule`, but may pass multiples of this number. Split large writes in the adapter if needed. Programming must only clear bits from `1` to `0` and FASTFFS will overwrite already programmed areas intentionally. FASTFFS handles blank checking when it expects empty space with read calls.
- `erase()` erases exactly the requested range to `0xff`, `off` will be aligned to sectors, and `len` will be a multiple of sector size. The driver may issue larger block erases to the flash if they are fully encompassed in the range. Pick a FASTFFS `sector_size` that your erase path can support without erasing neighboring FASTFFS sectors.
- Reject out-of-range operations and propagate driver failures by returning nonzero. FASTFFS maps any nonzero backend result to `FFFS_ERR_IO`.
- Ensure `FFFS_FILE_CACHE_SIZE >= program_granule`
- Ensure `backend.size` is divisible by `program_granule`
- Formatted `sector_size` divides `backend.size`.

`fffs_format()` and `fffs_mount()` validate these and fail with `FFFS_ERR_INVALID` when the backend violates them.

## Format

- Ideally format once during provisioning or factory reset.
- You can opt to format on mount failure to self-bootstrap at your peril. A blank/unformatted region and a damaged existing filesystem can both fail mount as `FFFS_ERR_CORRUPT` today.

- Choose an index sector count. Use more for increased wear leveling or many files. At most `(n-1) * 1000` files per 4K sector. Keep headroom below the hard cap: deletes and overwrites also append index records, and a filesystem at exactly the live-file capacity (1022 per usable 4K index sector) can no longer commit any namespace change, including deletes.
- Pick `sector_size` for your flash erase geometry, a power of two from 256 to 8192. It must divide `backend.size`, and be a multiple of your flash erase unit. `fffs_format()` erases the index/discovery area (at least the first 8 KiB), the rest will be GCed as needed during normal operation.

```c
int rc = fffs_format(&backend, &(struct fffs_format_options){
    .index_sectors = 2,          // 2..15
    .sector_size = FFFS_SECTOR_4K,
});
```

## Mount

Buffers provided to mount must be exclusively available to FFFS for the duration that the filesystem is mounted. This example is for a 2 MiB FASTFFS region, and 4 KiB sectors:

```c
static struct fffs fs;
static uint32_t index_cache[1024];
static uint32_t alloc_map[16];    //at least one bit per sector (512/32)
static uint8_t scratch[4096];    // one sector; can shrink to 64 bytes minimum if RAM is tight

int mount_fastffs(void) {
    return fffs_mount(&fs, &backend, &(struct fffs_mount_options){
        .index_cache = index_cache,
        .index_cache_size = sizeof(index_cache),
        .index_hash_table_size = 1024,
        .scratch = scratch,
        .scratch_size = sizeof(scratch),
        .strict = false,
        .alloc_map = alloc_map,
        .alloc_map_words = sizeof(alloc_map) / sizeof(alloc_map[0]),
    });
}
```

`index_hash_table_size` must be a power of two, and the default cache mode needs 4 bytes of `index_cache` per bucket; size it with `FFFS_INDEX_CACHE_BYTES(count)`. `scratch` must be at least 64 bytes.

One `alloc_map` word covers 32 formatted sectors. For a different storage size or sector size, change `alloc_map` to cover the formatted sector count.

## Use

Names are non-empty C strings up to `FFFS_MAX_NAME` bytes (32, fixed). `/` is just part of the name. An empty name fails with `FFFS_ERR_INVALID`, an oversized one with `FFFS_ERR_NAME_TOO_LONG`.

```c
struct fffs_file f;

int rc = fffs_open(&fs, &f, "cfg/net",
        FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC);
if (rc == FFFS_OK) {
    rc = fffs_write(&f, data, data_len);
}
if (rc == FFFS_OK) {
    rc = fffs_close(&f); // write commit point
}
```

`fffs_close()` commits the file to the filesystem.

```c
uint8_t buf[128];
size_t got;

rc = fffs_open(&fs, &f, "cfg/net", FFFS_O_RDONLY);
if (rc == FFFS_OK) {
    rc = fffs_read(&f, buf, sizeof(buf), &got);
    (void)fffs_close(&f);
}
```

```c
struct fffs_dir dir;
struct fffs_stat st;

rc = fffs_dir_open(&fs, &dir, "cfg/"); // simulate directory by scanning filename prefix
while (rc == FFFS_OK && fffs_dir_read(&dir, &st)) {
    app_do_something_with_file(st.name, st.size);
}
rc = rc == FFFS_OK ? fffs_dir_status(&dir) : rc;
(void)fffs_dir_close(&dir);
```

Call one or more GC steps from idle time, especially after deletes and overwrites:

```c
enum fffs_gc_action action;
(void)fffs_gc_step(&fs, &action);
```

Each step performs one bounded slice of work and reports it in `action`: scanning a sector for liveness, tombstoning dead data, or erasing a reclaimable sector. The GC cursor cycles the whole device continuously, so there is no terminal idle state to wait for. `fffs_gc(&fs, max_steps, &erased)` runs a batch of steps.
