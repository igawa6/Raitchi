/* NVN texture layout helpers derived from LAS Recomp Android
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * MIT: native/runtime/LICENSE-LASRecomp. Shared checked region upload extension. */
// Tegra X1 texture layout: formats and block-linear (GOB) swizzling.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t bpb;     // bytes per block (per texel for uncompressed)
    uint8_t bw, bh;  // block dimensions in texels
    uint8_t kind;    // FMT_COLOR, FMT_DEPTH, ...
    const char *name;
} FormatInfo;

enum { FMT_COLOR = 0, FMT_DEPTH = 1, FMT_STENCIL = 2, FMT_DEPTH_STENCIL = 3, FMT_COMPRESSED = 4 };

const FormatInfo *nvn_format_info(uint32_t nvn_format);
bool nvn_format_known(uint32_t nvn_format);

typedef struct {
    uint32_t width, height, depth, layers, levels;  // depth: 3D depth; layers: array layers / cube faces
    const FormatInfo *fmt;
    uint32_t block_height_log2;  // GOBs per block (log2) for level 0
    uint32_t block_depth_log2;
} TexLayout;

typedef struct {
    uint64_t offset;              // from the start of the layer
    uint32_t w, h, d;             // texel dimensions
    uint32_t wb, hb;              // in blocks
    uint32_t block_height_log2, block_depth_log2;
} LevelLayout;

// Default block height for a texture (what the NVN driver picks), and the full storage layout.
uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks);
void tegra_layout_init(TexLayout *t);
LevelLayout tegra_level(const TexLayout *t, uint32_t level);
uint64_t tegra_layer_size(const TexLayout *t);
uint64_t tegra_total_size(const TexLayout *t);

// NVN LINEAR and LINEAR_RENDER_TARGET both use pitched rows. Buffer target 10
// has its own tight-width rule and is intentionally not inferred from flags.
static inline bool nvn_texture_row_linear(uint32_t flags) { return (flags & 0x50u) != 0; }
// Supported pitched CPU layout: one level, one layer, one depth slice. Proves
// the complete stride*block-row storage span (including row padding).
bool nvn_texture_row_span(const TexLayout *layout, uint64_t stride, uint64_t *size);

// Copy a block-linear level slice into a linear buffer (row pitch = wb * bpb).
void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t block_height_log2, uint32_t block_depth_log2);
void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t block_height_log2, uint32_t block_depth_log2);

// Raw format blocks: coordinates are texels; z selects a 3D slice or array layer.
// Source strides are bytes (zero selects tightly packed rows/images). The
// destination's complete backing size is checked before any byte is changed.
typedef struct { uint32_t x, y, z, width, height, depth; } TexWriteRegion;
enum TexWriteResult { TEX_WRITE_OK, TEX_WRITE_ARGUMENT, TEX_WRITE_BOUNDS,
                      TEX_WRITE_FORMAT, TEX_WRITE_STRIDE, TEX_WRITE_STORAGE, TEX_WRITE_GPU_OWNED };
enum TexWriteResult tegra_write_region(const TexLayout *layout, uint32_t level,
    bool linear, uint64_t destination_stride, uint8_t *destination, uint64_t destination_size,
    const TexWriteRegion *region, const uint8_t *source, uint64_t row_stride, uint64_t image_stride);
