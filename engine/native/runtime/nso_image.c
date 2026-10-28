/*
 * NSO/LZ4 layout and match-copy semantics derived from LAS Recomp runtime/loader.c
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51; validation rewritten for owned buffers.
 * MIT License
 * Copyright (c) 2026 Peiikai
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "nso_image.h"
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static enum NsoImageStatus fail(NsoImageFailure *out, enum NsoImageStatus status, uint32_t segment) {
    if (out) *out = (NsoImageFailure){status, segment};
    return status;
}
static int nonzero(const uint8_t digest[32]) {
    for (unsigned i = 0; i < 32; ++i) if (digest[i]) return 1;
    return 0;
}

static int extension(const uint8_t *src, size_t size, size_t *at, size_t *length) {
    uint8_t byte;
    do {
        if (*at == size) return 0;
        byte = src[(*at)++];
        if (*length > SIZE_MAX - byte) return 0;
        *length += byte;
    } while (byte == 255);
    return 1;
}

/* dst==NULL is an internal validation-only pass. The offset/output-count
 * checks are identical to decoding, including overlapping matches. */
static enum NsoImageStatus lz4_block(const uint8_t *src, size_t size, uint8_t *dst,
                                   size_t capacity, size_t *written) {
    size_t at = 0, made = 0;
    *written = 0;
    while (at < size) {
        uint8_t token = src[at++];
        size_t literals = token >> 4;
        if (literals == 15 && !extension(src, size, &at, &literals)) return NSO_IMAGE_LZ4;
        if (literals > size - at || literals > capacity - made) return NSO_IMAGE_LZ4;
        if (dst && literals) memcpy(dst + made, src + at, literals);
        at += literals; made += literals;
        if (at == size) { *written = made; return NSO_IMAGE_OK; }
        if (size - at < 2) return NSO_IMAGE_LZ4;
        size_t offset = (size_t)src[at] | (size_t)src[at + 1] << 8;
        at += 2;
        if (!offset || offset > made) return NSO_IMAGE_LZ4;
        size_t match = token & 15;
        if (match == 15 && !extension(src, size, &at, &match)) return NSO_IMAGE_LZ4;
        if (match > SIZE_MAX - 4) return NSO_IMAGE_LZ4;
        match += 4;
        if (match > capacity - made) return NSO_IMAGE_LZ4;
        if (dst) for (size_t i = 0; i < match; ++i) dst[made + i] = dst[made + i - offset];
        made += match;
        /* A raw block ends in a literal sequence, never halfway after a match. */
        if (at == size) return NSO_IMAGE_LZ4;
    }
    *written = made;
    return NSO_IMAGE_OK;
}

enum NsoImageStatus nso_lz4_decompress(const uint8_t *src, size_t size,
    uint8_t *dst, size_t capacity, size_t *written) {
    if (written) *written = 0;
    if (!written || (!src && size) || (!dst && capacity)) return NSO_IMAGE_ARGUMENT;
    uintptr_t source = (uintptr_t)src, destination = (uintptr_t)dst;
    if (size > UINTPTR_MAX - source || capacity > UINTPTR_MAX - destination ||
        (size && capacity && source < destination + capacity && destination < source + size))
        return NSO_IMAGE_ARGUMENT;
    return lz4_block(src, size, dst, capacity, written);
}

/* Decoder needs only original image metadata. AOT table policy stays in its
 * public wrapper; the native wrapper never creates an AOT spec or table count. */
typedef struct ImageExpectation {
    uint32_t module_id;
    const uint8_t *nso_build_id;
    const uint8_t *nso_sha256;
    uint64_t canonical_base, mapped_size, text_offset, text_size;
} ImageExpectation;

static int expectation_valid(const ImageExpectation *spec, const ModuleAbiIdentity *id, uint32_t alignment) {
    if (!alignment || (alignment & (alignment - 1u)) || alignment < 4096 ||
        id->struct_size != sizeof *id ||
        id->abi_version != MODULE_ABI_VERSION || id->target != MODULE_ABI_TARGET_ANDROID_ARM64_LP64 ||
        id->cpu_alignment != 16 ||
        !((id->cpu_identity == MODULE_ABI_CPU_A64_PREFIX_1024 && id->cpu_size == 1024) ||
          ((id->cpu_identity == MODULE_ABI_CPU_A64_CACHE_66560 || id->cpu_identity == MODULE_ABI_CPU_A64_SHARED_66560) &&
           id->cpu_size == 66560)) ||
        !id->guest_window_lo || id->guest_window_lo >= id->guest_window_hi ||
        ((id->guest_shift | id->guest_window_lo | id->guest_window_hi) & 3) ||
        spec->module_id < MODULE_ABI_MAIN || spec->module_id > MODULE_ABI_SUBSDK1 ||
        !nonzero(spec->nso_build_id) || !nonzero(spec->nso_sha256) ||
        !spec->mapped_size || spec->mapped_size > SIZE_MAX || (spec->mapped_size & (alignment - 1u)) ||
        !spec->text_size || spec->text_offset > spec->mapped_size ||
        spec->text_size > spec->mapped_size - spec->text_offset ||
        ((spec->canonical_base | spec->text_offset | spec->text_size) & 3) ||
        spec->canonical_base < id->guest_shift) return 0;
    uint64_t actual = spec->canonical_base - id->guest_shift;
    return actual >= id->guest_window_lo && actual < id->guest_window_hi &&
           spec->mapped_size <= id->guest_window_hi - actual;
}

static enum NsoImageStatus decode_image(const uint8_t *raw, size_t raw_size,
    const ImageExpectation *expected, const ModuleAbiIdentity *identity,
    uint32_t alignment, NsoImage *out, NsoImageFailure *failure) {
    if (raw_size < NSO_IMAGE_HEADER_SIZE || memcmp(raw, "NSO0", 4) || u32(raw + 4) || u32(raw + 8) ||
        (u32(raw + 0xc) & ~0x3fu)) return fail(failure, NSO_IMAGE_HEADER, NSO_IMAGE_NO_SEGMENT);
    if (memcmp(raw + 0x40, expected->nso_build_id, 32))
        return fail(failure, NSO_IMAGE_BUILD_ID, NSO_IMAGE_NO_SEGMENT);
    NsoImage image = {.actual_base = expected->canonical_base - identity->guest_shift, .flags = u32(raw + 0xc)};
    memcpy(image.build_id, raw + 0x40, 32);
    uint64_t ends[NSO_IMAGE_SEGMENTS];
    for (uint32_t i = 0; i < NSO_IMAGE_SEGMENTS; ++i) {
        NsoImageSegment *segment = &image.segments[i];
        const uint8_t *header = raw + 0x10 + i * 0x10;
        segment->file_offset = u32(header);
        segment->memory_offset = u32(header + 4);
        segment->size = u32(header + 8);
        segment->compressed = (image.flags >> i) & 1u;
        segment->hash_requested = (image.flags >> (i + 3)) & 1u;
        segment->stored_size = segment->compressed ? u32(raw + 0x60 + i * 4) : (uint32_t)segment->size;
        memcpy(segment->header_sha256, raw + 0xa0 + i * 32, 32);
        if (segment->file_offset > raw_size || segment->stored_size > raw_size - segment->file_offset ||
            (segment->stored_size && segment->file_offset < NSO_IMAGE_HEADER_SIZE))
            return fail(failure, NSO_IMAGE_FILE_RANGE, i);
        if (segment->memory_offset > expected->mapped_size || segment->size > expected->mapped_size - segment->memory_offset)
            return fail(failure, NSO_IMAGE_MEMORY_RANGE, i);
        ends[i] = segment->memory_offset + segment->size;
        if (ends[i] > image.image_end) image.image_end = ends[i];
        for (uint32_t j = 0; j < i; ++j) {
            const NsoImageSegment *prior = &image.segments[j];
            if ((segment->size && prior->size && segment->memory_offset < ends[j] && prior->memory_offset < ends[i]) ||
                (segment->stored_size && prior->stored_size &&
                 (uint64_t)segment->file_offset < (uint64_t)prior->file_offset + prior->stored_size &&
                 (uint64_t)prior->file_offset < (uint64_t)segment->file_offset + segment->stored_size))
                return fail(failure, NSO_IMAGE_OVERLAP, i);
        }
    }
    if (image.segments[0].memory_offset != expected->text_offset || image.segments[0].size != expected->text_size)
        return fail(failure, NSO_IMAGE_EXPECTATION, NSO_SEGMENT_TEXT);
    image.bss_offset = ends[NSO_SEGMENT_DATA];
    image.bss_size = u32(raw + 0x3c);
    if (image.bss_size > expected->mapped_size - image.bss_offset)
        return fail(failure, NSO_IMAGE_MEMORY_RANGE, NSO_SEGMENT_DATA);
    uint64_t bss_end = image.bss_offset + image.bss_size;
    for (uint32_t i = 0; i < NSO_SEGMENT_DATA; ++i)
        if (image.bss_size && image.segments[i].size && image.bss_offset < ends[i] && image.segments[i].memory_offset < bss_end)
            return fail(failure, NSO_IMAGE_OVERLAP, i);
    image.data_size_with_bss = image.segments[NSO_SEGMENT_DATA].size + image.bss_size;
    if (bss_end > image.image_end) image.image_end = bss_end;
    if (image.image_end > UINT64_MAX - (alignment - 1u) ||
        ((image.image_end + alignment - 1u) & ~(uint64_t)(alignment - 1u)) != expected->mapped_size)
        return fail(failure, NSO_IMAGE_MAPPED_SIZE, NSO_IMAGE_NO_SEGMENT);
    for (uint32_t i = 0; i < NSO_IMAGE_SEGMENTS; ++i) if (image.segments[i].compressed) {
        size_t written;
        if (lz4_block(raw + image.segments[i].file_offset, image.segments[i].stored_size, NULL,
                      (size_t)image.segments[i].size, &written) != NSO_IMAGE_OK)
            return fail(failure, NSO_IMAGE_LZ4, i);
        if (written != image.segments[i].size) return fail(failure, NSO_IMAGE_DECODE_SIZE, i);
    }
    image.size = (size_t)expected->mapped_size;
    image.bytes = calloc(1, image.size);
    if (!image.bytes) return fail(failure, NSO_IMAGE_ALLOCATION, NSO_IMAGE_NO_SEGMENT);
    for (uint32_t i = 0; i < NSO_IMAGE_SEGMENTS; ++i) {
        const NsoImageSegment *segment = &image.segments[i];
        if (segment->compressed) {
            size_t written;
            /* Immutable input was validated with identical bounds before allocation. */
            if (lz4_block(raw + segment->file_offset, segment->stored_size, image.bytes + segment->memory_offset,
                          (size_t)segment->size, &written) != NSO_IMAGE_OK || written != segment->size) {
                free(image.bytes); return fail(failure, NSO_IMAGE_LZ4, i);
            }
        } else if (segment->size) memcpy(image.bytes + segment->memory_offset, raw + segment->file_offset, (size_t)segment->size);
    }
    *out = image;
    return NSO_IMAGE_OK;
}

enum NsoImageStatus nso_image_decode(const uint8_t *raw, size_t raw_size,
    const ModuleAbiModuleSpec *expected, const ModuleAbiIdentity *identity,
    uint32_t alignment, NsoImage *out, NsoImageFailure *failure) {
    if (out) *out = (NsoImage){0};
    fail(failure, NSO_IMAGE_OK, NSO_IMAGE_NO_SEGMENT);
    if (!raw || !expected || !identity || !out) return fail(failure, NSO_IMAGE_ARGUMENT, NSO_IMAGE_NO_SEGMENT);
    const ImageExpectation image = {expected->module_id, expected->nso_build_id, expected->nso_sha256,
        expected->canonical_base, expected->mapped_size, expected->text_offset, expected->text_size};
    if (expected->struct_size != sizeof *expected || !expected->function_count ||
        expected->function_count > SIZE_MAX / sizeof(ModuleAbiGuestFn) ||
        expected->function_count > expected->text_size / 4 || !expectation_valid(&image, identity, alignment))
        return fail(failure, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    return decode_image(raw, raw_size, &image, identity, alignment, out, failure);
}

enum NsoImageStatus nso_image_decode_native(const uint8_t *raw, size_t raw_size,
    const NativeModuleSpec *expected, const ModuleAbiIdentity *identity,
    uint32_t alignment, NsoImage *out, NsoImageFailure *failure) {
    if (out) *out = (NsoImage){0};
    fail(failure, NSO_IMAGE_OK, NSO_IMAGE_NO_SEGMENT);
    if (!raw || !expected || !identity || !out) return fail(failure, NSO_IMAGE_ARGUMENT, NSO_IMAGE_NO_SEGMENT);
    const ImageExpectation image = {expected->module_id, expected->nso_build_id, expected->nso_sha256,
        expected->canonical_base, expected->mapped_size, expected->text_offset, expected->text_size};
    if (expected->struct_size != sizeof *expected || alignment > 65536 ||
        identity->cpu_identity != MODULE_ABI_CPU_A64_SHARED_66560 ||
        !expectation_valid(&image, identity, alignment) ||
        ((expected->canonical_base | identity->guest_shift | identity->guest_window_lo |
            identity->guest_window_hi) & (alignment - 1u)) ||
        identity->guest_window_hi > UINT64_MAX - identity->guest_shift ||
        expected->mapped_size > UINT64_MAX - expected->canonical_base)
        return fail(failure, NSO_IMAGE_EXPECTATION, NSO_IMAGE_NO_SEGMENT);
    return decode_image(raw, raw_size, &image, identity, alignment, out, failure);
}

void nso_image_destroy(NsoImage *image) {
    if (image) { free(image->bytes); *image = (NsoImage){0}; }
}
const char *nso_image_status_string(enum NsoImageStatus status) {
    switch (status) {
    case NSO_IMAGE_OK: return "ok";
    case NSO_IMAGE_ARGUMENT: return "invalid decoder argument";
    case NSO_IMAGE_HEADER: return "invalid or unsupported NSO0 header";
    case NSO_IMAGE_EXPECTATION: return "independent module expectations rejected";
    case NSO_IMAGE_BUILD_ID: return "full NSO build ID mismatch";
    case NSO_IMAGE_FILE_RANGE: return "NSO segment outside file payload";
    case NSO_IMAGE_MEMORY_RANGE: return "NSO segment or BSS outside expected mapping";
    case NSO_IMAGE_OVERLAP: return "overlapping NSO segment or BSS ranges";
    case NSO_IMAGE_MAPPED_SIZE: return "decoded mapping size mismatch";
    case NSO_IMAGE_LZ4: return "malformed or oversized LZ4 block";
    case NSO_IMAGE_DECODE_SIZE: return "LZ4 output length mismatch";
    case NSO_IMAGE_ALLOCATION: return "owned NSO image allocation failed";
    default: return "unknown NSO decode status";
    }
}
