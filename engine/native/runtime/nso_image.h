/* Strict image decode only: no NSO mapping, MOD0, symbols, relocations or entry. */
#ifndef SHARED_RECOMP_NSO_IMAGE_H
#define SHARED_RECOMP_NSO_IMAGE_H
#include "../module_abi.h"
#include "native_manifest.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NSO_IMAGE_API __attribute__((visibility("default")))
#define NSO_IMAGE_HEADER_SIZE 0x100u
#define NSO_IMAGE_SEGMENTS 3u
#define NSO_IMAGE_NO_SEGMENT UINT32_MAX
enum NsoSegmentId { NSO_SEGMENT_TEXT, NSO_SEGMENT_RODATA, NSO_SEGMENT_DATA };
enum NsoImageStatus {
    NSO_IMAGE_OK, NSO_IMAGE_ARGUMENT, NSO_IMAGE_HEADER, NSO_IMAGE_EXPECTATION,
    NSO_IMAGE_BUILD_ID, NSO_IMAGE_FILE_RANGE, NSO_IMAGE_MEMORY_RANGE,
    NSO_IMAGE_OVERLAP, NSO_IMAGE_MAPPED_SIZE, NSO_IMAGE_LZ4,
    NSO_IMAGE_DECODE_SIZE, NSO_IMAGE_ALLOCATION
};
typedef struct NsoImageSegment {
    uint64_t memory_offset;
    uint64_t size;
    uint32_t file_offset;
    uint32_t stored_size;
    uint32_t compressed;
    uint32_t hash_requested;
    uint8_t header_sha256[32]; /* recorded only; NOT verified by this decoder */
} NsoImageSegment;
typedef struct NsoImage {
    uint8_t *bytes; /* owned calloc buffer, including zero BSS/gaps/mapping padding */
    size_t size;
    uint64_t actual_base;
    uint64_t image_end; /* highest segment/BSS endpoint, before mapping alignment */
    uint64_t bss_offset;
    uint64_t bss_size;
    uint64_t data_size_with_bss;
    uint32_t flags;
    uint8_t build_id[32];
    NsoImageSegment segments[NSO_IMAGE_SEGMENTS];
} NsoImage;
typedef struct NsoImageFailure {
    enum NsoImageStatus status;
    uint32_t segment_index;
} NsoImageFailure;

/* Independently trusted expected metadata supplies the allocation bound and
 * actual address window. alignment is an explicit mapping policy (power of two,
 * >=4096), not the host's page size. Rounded decoded size must equal mapped_size.
 * NSO0 version0, known flags, full32byte ID, file/memory ranges, overlaps and all
 * compressed blocks are checked BEFORE allocating/decoding the owned image.
 * raw/spec/identity must remain readable+immutable; out must be empty and must
 * not alias inputs. On failure out is cleared, no image is retained. Caller
 * verifies the whole original-file SHA256 before this call; neither expected
 * nso_sha256 nor optional NSO segment hashes are cryptographically verified here.
 * Does not reserve/write guest memory. Caller frees bytes or uses destroy(). */
NSO_IMAGE_API enum NsoImageStatus nso_image_decode(const uint8_t *raw, size_t raw_size,
    const ModuleAbiModuleSpec *expected, const ModuleAbiIdentity *identity,
    uint32_t alignment, NsoImage *out, NsoImageFailure *failure);

/* Native-only original image expectation: no AOT descriptor, function count or
 * native_manifest.c linkage. Uses the same NSO/LZ4/owned-output decoder above.
 * Requires the shared CPU3 identity and mapping alignment 4096..65536, with
 * page-aligned canonical base/actual window/shift, checked canonical endpoints
 * and nonblank original build/file identities. Caller verifies the full original
 * file SHA256, cross-module overlaps and accepted title/runtime contract before
 * this call (native_manifest_validate handles metadata, not file hashing).
 * All decode ownership/immutability/output requirements above apply unchanged. */
NSO_IMAGE_API enum NsoImageStatus nso_image_decode_native(const uint8_t *raw, size_t raw_size,
    const NativeModuleSpec *expected, const ModuleAbiIdentity *identity,
    uint32_t alignment, NsoImage *out, NsoImageFailure *failure);
NSO_IMAGE_API void nso_image_destroy(NsoImage *image);

/* Bounds-safe raw LZ4 block decoder. Source/destination must be valid,
 * nonoverlapping buffers; overlapping match copies WITHIN dst are supported.
 * Returns produced size on success (may be below capacity), clears written on
 * failure. Partial dst bytes may be written on failure: no atomic-output claim.
 * This checks block bounds/offsets, not canonical compressor ending heuristics. */
NSO_IMAGE_API enum NsoImageStatus nso_lz4_decompress(const uint8_t *src, size_t src_size,
    uint8_t *dst, size_t dst_capacity, size_t *written);
NSO_IMAGE_API const char *nso_image_status_string(enum NsoImageStatus status);
#ifdef __cplusplus
}
#endif
#endif
