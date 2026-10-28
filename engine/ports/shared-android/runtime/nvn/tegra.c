/* NVN texture layout helpers derived from LAS Recomp Android
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * MIT: native/runtime/LICENSE-LASRecomp. Shared checked region upload extension. */
// Tegra X1 block-linear layout. A GOB is 64 bytes x 8 rows; blocks stack GOBs vertically
// (block height) and in depth (block depth). Levels shrink their block height as they shrink.
#include "tegra.h"

#include <string.h>

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint64_t align_up(uint64_t a, uint64_t b) { return (a + b - 1) / b * b; }

uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks) {
    uint32_t gobs = div_up(height_in_blocks, 8);
    uint32_t log2 = 0;
    while ((1u << log2) < gobs && log2 < 4) log2++;
    return log2;
}

static uint32_t block_depth_for(uint32_t depth) {
    uint32_t log2 = 0;
    while ((1u << log2) < depth && log2 < 5) log2++;
    return log2;
}

void tegra_layout_init(TexLayout *t) {
    uint32_t hb = div_up(t->height, t->fmt->bh);
    t->block_height_log2 = tegra_default_block_height_log2(hb);
    t->block_depth_log2 = t->depth > 1 ? block_depth_for(t->depth) : 0;
}

// Block height shrinks with the level so a block never exceeds the level height.
static uint32_t level_block_height(uint32_t bh_log2, uint32_t hb) {
    while (bh_log2 > 0 && (8u << (bh_log2 - 1)) >= hb) bh_log2--;
    return bh_log2;
}

static uint32_t level_block_depth(uint32_t bd_log2, uint32_t d) {
    while (bd_log2 > 0 && (1u << (bd_log2 - 1)) >= d) bd_log2--;
    return bd_log2;
}

static uint64_t level_size(const TexLayout *t, uint32_t level, LevelLayout *out) {
    uint32_t w = t->width >> level, h = t->height >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint32_t wb = div_up(w, t->fmt->bw), hb = div_up(h, t->fmt->bh);
    uint32_t bh = level_block_height(t->block_height_log2, hb);
    uint32_t bd = level_block_depth(t->block_depth_log2, d);
    uint32_t gob_rows = 8u << bh, gob_depth = 1u << bd;
    uint64_t width_gobs = div_up(wb * t->fmt->bpb, 64);
    uint64_t size = width_gobs * 512 * (1u << bh) * (1u << bd) * div_up(hb, gob_rows) * div_up(d, gob_depth);
    if (out) {
        out->w = w; out->h = h; out->d = d;
        out->wb = wb; out->hb = hb;
        out->block_height_log2 = bh;
        out->block_depth_log2 = bd;
    }
    return size;
}

LevelLayout tegra_level(const TexLayout *t, uint32_t level) {
    LevelLayout l = {0};
    uint64_t off = 0;
    for (uint32_t i = 0; i < level; i++) off += level_size(t, i, NULL);
    level_size(t, level, &l);
    l.offset = off;
    return l;
}

uint64_t tegra_layer_size(const TexLayout *t) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < t->levels; i++) size += level_size(t, i, NULL);
    if (t->layers > 1) {
        // layers are aligned to the level-0 block size
        uint32_t bh = t->block_height_log2;
        size = align_up(size, 512ull << bh);
    }
    return size;
}

uint64_t tegra_total_size(const TexLayout *t) {
    return tegra_layer_size(t) * (t->layers ? t->layers : 1);
}

bool nvn_texture_row_span(const TexLayout *t, uint64_t stride, uint64_t *size) {
    if (!size) return false;
    *size = 0;
    const FormatInfo *f = t ? t->fmt : NULL;
    if (!f || !f->bpb || !f->bw || !f->bh || !t->width || !t->height ||
        t->levels != 1 || t->layers > 1 || t->depth != 1) return false;
    uint64_t rows = ((uint64_t)t->height + f->bh - 1) / f->bh;
    uint64_t columns = ((uint64_t)t->width + f->bw - 1) / f->bw;
    if (stride < columns * f->bpb || stride > UINT64_MAX / rows) return false;
    *size = stride * rows;
    return true;
}

// Offset of byte (x bytes, y rows, z slices) within a block-linear surface.
static inline uint64_t gob_offset(uint32_t xb, uint32_t y, uint32_t z, uint32_t width_gobs, uint32_t bh_log2,
                                  uint32_t bd_log2, uint64_t slice_blocks_rows) {
    uint32_t gob_x = xb >> 6, gob_y = y >> 3;
    uint32_t block_y = gob_y >> bh_log2, gob_in_block_y = gob_y & ((1u << bh_log2) - 1);
    uint32_t block_z = z >> bd_log2, z_in_block = z & ((1u << bd_log2) - 1);
    uint64_t block_size = 512ull << (bh_log2 + bd_log2);
    uint64_t off = ((uint64_t)block_z * slice_blocks_rows + block_y) * width_gobs * block_size;
    off += (uint64_t)gob_x * block_size;
    off += ((uint64_t)z_in_block << bh_log2 | gob_in_block_y) * 512;
    // within the GOB: 64x8 bytes arranged in 16-byte sectors
    uint32_t xi = xb & 63, yi = y & 7;
    off += ((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 0x0F);
    return off;
}

static void copy_surface(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                         uint32_t bh_log2, uint32_t bd_log2, bool to_linear) {
    uint32_t row_bytes = wb * bpb;
    uint32_t width_gobs = (row_bytes + 63) / 64;
    uint32_t gob_rows = 8u << bh_log2;
    uint64_t blocks_rows = (hb + gob_rows - 1) / gob_rows;
    for (uint32_t z = 0; z < d; z++) {
        for (uint32_t y = 0; y < hb; y++) {
            uint8_t *lin = dst + ((uint64_t)z * hb + y) * row_bytes;
            const uint8_t *lin_src = src + ((uint64_t)z * hb + y) * row_bytes;
            // 16-byte runs are contiguous in both layouts
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint64_t o = gob_offset(x, y, z, width_gobs, bh_log2, bd_log2, blocks_rows);
                if (to_linear) memcpy(lin + x, src + o, n);
                else memcpy(dst + o, lin_src + x, n);
            }
        }
    }
}

void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, true);
}

void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, false);
}

// Write only the requested physical blocks. A read/modify/swizzle of the whole
// texture would also rewrite padding, other mips and concurrent unrelated data.
enum TexWriteResult tegra_write_region(const TexLayout *t, uint32_t level,
    bool linear, uint64_t pitch, uint8_t *dst, uint64_t dst_size,
    const TexWriteRegion *r, const uint8_t *src, uint64_t row_pitch, uint64_t image_pitch) {
    if (!t || !r || !dst || !src || !t->fmt || !t->width || !t->height || !t->depth ||
        !t->levels || t->levels > 17 || level >= t->levels || !r->width || !r->height || !r->depth)
        return TEX_WRITE_ARGUMENT;
    const FormatInfo *f = t->fmt;
    if (!f->bpb || f->bpb > 16 || !f->bw || !f->bh || f->bw > 12 || f->bh > 12)
        return TEX_WRITE_FORMAT;
    // Generous bounds above NVN texture limits also keep legacy layout math and
    // shifts representable. A texture cannot have both array layers and 3D depth.
    uint32_t layers = t->layers ? t->layers : 1;
    if (t->width > 65536 || t->height > 65536 || t->depth > 2048 || layers > 65536 ||
        (layers > 1 && t->depth > 1) || t->block_height_log2 > 5 || t->block_depth_log2 > 5)
        return TEX_WRITE_BOUNDS;
    LevelLayout l = tegra_level(t, level);
    uint32_t slices = t->depth > 1 ? l.d : layers;
    if (r->x >= l.w || r->width > l.w - r->x || r->y >= l.h || r->height > l.h - r->y ||
        r->z >= slices || r->depth > slices - r->z) return TEX_WRITE_BOUNDS;
    // Compressed updates cover complete format blocks, except at the mip edge.
    if (r->x % f->bw || r->y % f->bh ||
        (r->width % f->bw && r->x + r->width != l.w) ||
        (r->height % f->bh && r->y + r->height != l.h)) return TEX_WRITE_FORMAT;
    uint32_t xbytes = (r->x / f->bw) * f->bpb, yblocks = r->y / f->bh;
    uint32_t rows = (r->height + f->bh - 1) / f->bh;
    uint32_t row_bytes = ((r->width + f->bw - 1) / f->bw) * f->bpb;
    if (!row_pitch) row_pitch = row_bytes;
    if (row_pitch < row_bytes || row_pitch > SIZE_MAX / rows) return TEX_WRITE_STRIDE;
    uint64_t minimum_image = row_pitch * rows;
    if (!image_pitch) image_pitch = minimum_image;
    if (image_pitch < (uint64_t)(rows - 1) * row_pitch + row_bytes || image_pitch > SIZE_MAX / r->depth)
        return TEX_WRITE_STRIDE;
    uint64_t source_span = (uint64_t)(r->depth - 1) * image_pitch + (uint64_t)(rows - 1) * row_pitch + row_bytes;
    if (source_span > UINTPTR_MAX - (uintptr_t)src) return TEX_WRITE_STRIDE;
    uint64_t layer_size = tegra_layer_size(t), needed = tegra_total_size(t);
    if (linear) {
        if (t->levels != 1 || t->depth != 1 || layers != 1) return TEX_WRITE_FORMAT;
        if (pitch < (uint64_t)l.wb * f->bpb || pitch > UINT64_MAX / l.hb) return TEX_WRITE_STRIDE;
        needed = pitch * l.hb;
    }
    if (needed > dst_size || needed > UINTPTR_MAX - (uintptr_t)dst) return TEX_WRITE_STORAGE;
    // NVN client buffers are independent of texture storage. Reject overlap
    // rather than allowing GOB-order stores to clobber unread source bytes.
    if ((uintptr_t)src < (uintptr_t)dst + needed && (uintptr_t)dst < (uintptr_t)src + source_span)
        return TEX_WRITE_ARGUMENT;
    uint32_t width_gobs = (l.wb * f->bpb + 63) / 64;
    uint64_t block_rows = (l.hb + (8u << l.block_height_log2) - 1) / (8u << l.block_height_log2);
    for (uint32_t z = 0; z < r->depth; z++) {
        uint32_t slice = t->depth > 1 ? r->z + z : 0;
        uint64_t layer = t->depth > 1 ? 0 : (uint64_t)(r->z + z) * layer_size;
        for (uint32_t y = 0; y < rows; y++) {
            const uint8_t *row = src + (uint64_t)z * image_pitch + (uint64_t)y * row_pitch;
            if (linear) { memcpy(dst + (uint64_t)(yblocks + y) * pitch + xbytes, row, row_bytes); continue; }
            for (uint32_t x = 0; x < row_bytes;) {
                uint32_t xb = xbytes + x, n = 16 - (xb & 15);
                if (n > row_bytes - x) n = row_bytes - x;
                uint64_t offset = gob_offset(xb, yblocks + y, slice, width_gobs,
                                            l.block_height_log2, l.block_depth_log2, block_rows);
                memcpy(dst + layer + l.offset + offset, row + x, n);
                x += n;
            }
        }
    }
    return TEX_WRITE_OK;
}
