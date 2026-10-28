/* Source locations only. Original shader bytes remain in the owner's image. */
#pragma once
#include "../nxpkg.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ShaderSourceIndex ShaderSourceIndex;
typedef struct ShaderSourceSpan {
    uint64_t hash, offset;
    uint32_t size, const_offset, const_size;
    /* BNSH stage bits: VS0,TCS1,TES2,GS3,FS4,CS5. */
    uint8_t stage_mask;
} ShaderSourceSpan;
ShaderSourceIndex *shader_source_index_create(void);
void shader_source_index_destroy(ShaderSourceIndex *index);
size_t shader_source_index_count(const ShaderSourceIndex *index);
/* Index BNSH stages in main NSO read-only/data segments. Original verified ExeFS
 * input, no output file. Decompression is bounded to64MiB/segment; only one
 * segment per image is cached at a time. Caller must serialize index APIs. */
bool shader_source_index_nso(ShaderSourceIndex *index, const char *path,
                             const volatile int *cancel, char *why, size_t why_size);

/* Extracted-folder equivalent, no symlink traversal. The index owns the root
 * descriptor and bounded per-file locations until destruction. */
bool shader_source_index_directory(ShaderSourceIndex *index, const char *root,
                                   const volatile int *cancel, char *why, size_t why_size);

/* Sources and contexts are borrowed until index destruction. Only metadata and
 * locations persist. Scans use <=64KiB+7-byte reads and never buffer whole archives.
 * Hard caps:65536 stage entries,4096 sources,1GiB scanned,16MiB/stage and
 * 256MiB/container. Partial results are usable after cancellation/failure, but
 * callers should discard the index if complete inventory is required. */
bool shader_source_index_scan(ShaderSourceIndex *index, NxReadAt read, void *context,
                              uint64_t source_size, uint64_t offset, uint64_t length,
                              const volatile int *cancel, char *why, size_t why_size);
bool shader_source_index_romfs(ShaderSourceIndex *index, NxRomfs *romfs,
                               const volatile int *cancel, char *why, size_t why_size);
/* Generic-reader RomFS variant supports synthetic and other seekable images. */
bool shader_source_index_romfs_reader(ShaderSourceIndex *index, NxReadAt read, void *context,
                                      uint64_t image_size, const volatile int *cancel,
                                      char *why, size_t why_size);
bool shader_source_index_lookup(const ShaderSourceIndex *index, uint64_t hash, ShaderSourceSpan *span);
/* Fresh caller-owned bytes,size+0x100 padding; rehashed after reading. NULL on
 * missing/corrupt sources, incompatible exact stage/partition or allocation error. */
uint8_t *shader_source_index_read(const ShaderSourceIndex *index, uint64_t hash, ShaderSourceSpan *span);
uint8_t *shader_source_index_read_exact(const ShaderSourceIndex *index, uint64_t hash, uint8_t stage,
                                       uint32_t code_bytes, uint32_t constant_bytes, ShaderSourceSpan *span);
#ifdef __cplusplus
}
#endif
