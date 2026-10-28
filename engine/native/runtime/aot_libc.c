/*
 * Extracted from LAS Recomp runtime/hle/libc.c, pinned Android reference
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51. SDK helper semantics preserved.
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
#include "aot_libc.h"
#include <stdint.h>
#include <string.h>

#define P(r) ((void *)(uintptr_t)c->x[r])
#define CP(r) ((const uint8_t *)(uintptr_t)c->x[r])

static int64_t byte_diff(const uint8_t *a, const uint8_t *b) { /* a and b differ somewhere ahead */
    while (*a == *b) a++, b++;
    return (int64_t)*a - (int64_t)*b;
}

void hle_memcpy(Cpu *c) { memmove(P(0), P(1), c->x[2]); }
void hle_memmove(Cpu *c) { memmove(P(0), P(1), c->x[2]); }
void hle_memset(Cpu *c) { memset(P(0), (int)(uint8_t)c->x[1], c->x[2]); }
void hle_memcmp(Cpu *c) { c->x[0] = (uint64_t)(memcmp(P(0), P(1), c->x[2]) ? byte_diff(CP(0), CP(1)) : 0); }
void hle_bcmp(Cpu *c) { c->x[0] = memcmp(P(0), P(1), c->x[2]) != 0; }
void hle_strlen(Cpu *c) { c->x[0] = strlen((const char *)P(0)); }
void hle_strcmp(Cpu *c) {
    const uint8_t *a = CP(0), *b = CP(1);
    while (*a && *a == *b) a++, b++;
    c->x[0] = (uint64_t)((int64_t)*a - (int64_t)*b);
}
void hle_strncmp(Cpu *c) {
    const uint8_t *a = CP(0), *b = CP(1);
    uint64_t n = c->x[2];
    if (!n) { c->x[0] = 0; return; }
    while (--n && *a && *a == *b) a++, b++;
    c->x[0] = (uint64_t)((int64_t)*a - (int64_t)*b);
}
void hle_memchr(Cpu *c) { c->x[0] = (uint64_t)(uintptr_t)memchr(P(0), (int)(uint8_t)c->x[1], c->x[2]); }
