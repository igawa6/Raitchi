// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
/* Checked portable adaptation of audio renderer component accounting from Eden
 * c6541721fa6307e99b3fdd9517dd4ed1b73927ed, audio_core/renderer/system.cpp,
 * command/command_generator.h, splitter/splitter_context.cpp and performance/
 * performance_manager.h. Original upstream notices: GPL-3.0-or-later (Eden),
 * GPL-2.0-or-later (yuzu); this adaptation uses GPL-3.0-or-later.
 *
 * Numeric target-ABI sizes/alignment/feature transitions are independently
 * checked against the owned SDK server sizing oracle for REV1..REV11. They are
 * intentionally not host-class sizeof values: current Eden host layouts and
 * some accounting terms differ. No original SDK instructions are included.
 * See LICENSE-audren-work-size.txt for the license text. */
#include "audren_work_size.h"
#include <stdbool.h>
#include <limits.h>

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool add(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) return false;
    *out = a + b; return true;
}
static bool multiply(uint64_t a, uint64_t b, uint64_t *out) {
    if (b && a > UINT64_MAX / b) return false;
    *out = a * b; return true;
}
static bool align(uint64_t value, uint64_t alignment, uint64_t *out) {
    if (!add(value, alignment - 1, out)) return false;
    *out &= ~(alignment - 1); return true;
}
#define CHECKED(expression) do { if (!(expression)) return AUDREN_WORK_SIZE_OVERFLOW; } while (0)
#define APPEND(value) CHECKED(add(size, (value), &size))

enum AudrenWorkSizeResult audren_work_size(const void *parameters, size_t length, uint64_t *out_size) {
    if (!out_size) return AUDREN_WORK_SIZE_ARGUMENT;
    *out_size = 0;
    if (!parameters || length != AUDREN_WORK_PARAMETER_SIZE) return AUDREN_WORK_SIZE_ARGUMENT;
    const uint8_t *p = parameters;
    if (p[48] != 'R' || p[49] != 'E' || p[50] != 'V' || p[51] < '1' || p[51] > ';')
        return AUDREN_WORK_SIZE_REVISION;
    unsigned revision = p[51] - '0';
    // SDK loads these as signed counts. Do not reproduce signed wraparound on
    // malformed inputs even though the manager itself checks only revision.
    for (unsigned offset = 4; offset <= 28; offset += 4)
        if (le32(p + offset) > INT32_MAX) return AUDREN_WORK_SIZE_PARAMETER;
    if (le32(p + 36) > INT32_MAX || le32(p + 40) > INT32_MAX) return AUDREN_WORK_SIZE_PARAMETER;
    uint64_t samples = le32(p + 4), mixes = le32(p + 8), submixes = le32(p + 12);
    uint64_t voices = le32(p + 16), sinks = le32(p + 20), effects = le32(p + 24);
    uint64_t perf_frames = le32(p + 28), splitters = le32(p + 36), destinations = le32(p + 40);
    uint64_t nodes = submixes + 1, outputs = sinks + submixes;
    uint64_t size = 0, part, temporary;

    // Mix/voice state, pointer lists, depop and sample buffers.
    CHECKED(align(mixes * 4, 64, &part)); APPEND(part);
    APPEND(submixes * 1024);
    APPEND(nodes * 0x940);
    APPEND(voices * 0x3f0);
    CHECKED(align(nodes * 8, 16, &part)); APPEND(part);
    CHECKED(align(voices * 8, 16, &part)); APPEND(part);
    CHECKED(multiply(outputs * 960 + samples * 4, mixes + 6, &part));
    CHECKED(align(part, 64, &part)); APPEND(part);

    if (revision >= 2) {
        // The original graph's n*n and rounded bit counts are signed 32-bit.
        // Refuse their overflow rather than extrapolating a different 64-bit ABI.
        if (nodes * nodes > INT32_MAX - 63u) return AUDREN_WORK_SIZE_OVERFLOW;
        uint64_t square = nodes * nodes, node_bits, edge_bits;
        CHECKED(align(nodes, 64, &node_bits));
        CHECKED(align(square, 64, &edge_bits));
        part = square * 4 + nodes * 12 + node_bits / 4 + edge_bits / 8;
        CHECKED(align(part, 16, &part)); APPEND(part);
        APPEND(destinations * 0xe0 + splitters * 0x20);
        // Pinned SDK's old (REV2..4) splitter accounting includes this index
        // array; REV5's corrected splitter path omits it.
        if (revision < 5) { CHECKED(align(destinations * 4, 16, &part)); APPEND(part); }
    }
    APPEND((effects + voices * 4) * 0x30); // memory pools
    if (revision >= 9) APPEND(effects * 0x80); // CPU effect results
    APPEND(0x50);
    CHECKED(align(size, 64, &size));
    APPEND(outputs * 0x280); // upsamplers
    APPEND(effects * 0x4c0); // effect state
    if (revision >= 9) APPEND(effects * 0x80); // DSP effect results
    APPEND(voices * 0x100); // DSP voice state
    APPEND(sinks * 0x170);
    APPEND(0x40);

    uint64_t entries = voices + effects + submixes + sinks + 1;
    if (perf_frames) {
        if (entries > INT32_MAX) return AUDREN_WORK_SIZE_OVERFLOW;
        uint64_t frame = revision >= 5 ? entries * 24 + 0x990 : entries * 16 + 0x658;
        CHECKED(multiply(frame, perf_frames + 1, &part));
        CHECKED(add(part, 0xc0, &part));
        CHECKED(align(part, 64, &part)); APPEND(part);
    }
    if (revision >= 5) {
        // Worst-case DSP command capacity, including per-submix mixing, all
        // splitter routes, both filters and fixed performance-detail entries.
        if (entries + 100 > INT32_MAX) return AUDREN_WORK_SIZE_OVERFLOW;
        CHECKED(multiply(voices, splitters * 104 + (revision >= 8 ? 776 : 736), &part));
        temporary = effects * 2112 + submixes * 23096 + sinks * 328 + destinations * 1344;
        CHECKED(add(part, temporary, &part));
        CHECKED(add(part, (entries + 100) * 64 + 1016, &part));
    } else {
        part = 0x18000;
    }
    CHECKED(add(part, 126, &part)); APPEND(part);
    CHECKED(align(size, 4096, &size));
    if (size > INT64_MAX) return AUDREN_WORK_SIZE_OVERFLOW;
    *out_size = size;
    return AUDREN_WORK_SIZE_OK;
}
