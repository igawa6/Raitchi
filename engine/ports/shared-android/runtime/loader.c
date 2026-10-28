/* Derived from LAS Recomp runtime/loader.c, pinned
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51. Copyright (c) 2026 Peiikai.
 * MIT license: native/runtime/LICENSE-LASRecomp (included with this runtime).
 * Selected-title loading and checked staging replace the fixed two-module path.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "common_session.h"
#include "runtime/nso_image.h"
#include "runtime/aot_dispatch.h"
#include "runtime/aot_libc.h"

Module g_modules[4];
int g_nmodules;
enum { DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5,
       DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_RELAENT = 9, DT_STRSZ = 10,
       DT_SYMENT = 11, DT_INIT = 12, DT_FINI = 13, DT_REL = 17, DT_RELSZ = 18,
       DT_RELENT = 19, DT_PLTREL = 20, DT_JMPREL = 23, DT_INIT_ARRAY = 25,
       DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28,
       DT_RELRSZ = 35, DT_RELR = 36, DT_RELRENT = 37, DT_GNU_HASH = 0x6ffffef5 };
enum { R_NONE = 0, R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };
typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64Sym;
typedef struct { uint64_t off, info; int64_t addend; } Elf64Rela;
typedef struct { uint64_t off, value; } RelrWrite;
/* Bound expansion independently of the compressed table's file-size bound. */
enum { MAX_RELR_WRITES = 16 * 1024 * 1024 };
enum { MAX_GNU_HASH_WORDS = 16 * 1024 * 1024 };
_Static_assert(sizeof(Elf64Sym) == 24, "AArch64 dynsym layout");
_Static_assert(sizeof(Elf64Rela) == 24, "AArch64 RELA layout");
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "shared ARM64 loader requires a little-endian host"
#endif

typedef struct {
    NsoImage image;
    Module module;
    Elf64Sym *symbols; /* immutable host metadata; relocations cannot overwrite it */
    char *strings;
    size_t string_size;
    Elf64Rela *relocations;
    uint64_t *relocation_values;
    size_t relocation_count;
    RelrWrite *relr_writes;
    size_t relr_count;
} LoadedModule;
static LoadedModule loaded[4];
static unsigned loaded_count;
static bool started;

size_t rt_lz4_decompress(const uint8_t *src, size_t source_size, uint8_t *dst, size_t capacity) {
    size_t written = 0;
    return nso_lz4_decompress(src, source_size, dst, capacity, &written) == NSO_IMAGE_OK ? written : 0;
}
static uint32_t read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static uint64_t read64(const uint8_t *p) { uint64_t v; memcpy(&v, p, sizeof v); return v; }
static int64_t read_signed32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }

static bool in_image(const LoadedModule *m, uint64_t off, uint64_t count) {
    return off <= m->module.size && count <= m->module.size - off;
}
/* Metadata must live in initialized segment bytes, not zero padding/BSS. */
static uint64_t backed_tail(const LoadedModule *m, uint64_t off) {
    for (unsigned i = 0; i < NSO_IMAGE_SEGMENTS; ++i) {
        const NsoImageSegment *s = &m->image.segments[i];
        if (off >= s->memory_offset && off - s->memory_offset < s->size)
            return s->size - (off - s->memory_offset);
    }
    return 0;
}
static const uint8_t *metadata(const LoadedModule *m, uint64_t off, uint64_t count) {
    if (!count || count > backed_tail(m, off)) FATAL("%s: metadata outside initialized image (%llx+%llx)\n",
        m->module.name, (unsigned long long)off, (unsigned long long)count);
    return m->image.bytes + off;
}
static uint64_t relative_offset(const LoadedModule *m, uint64_t origin, int64_t displacement) {
    uint64_t magnitude = displacement < 0 ? (uint64_t)(-(displacement + 1)) + 1 : (uint64_t)displacement;
    if ((displacement < 0 && magnitude > origin) ||
        (displacement >= 0 && magnitude > m->image.size - origin))
        FATAL("%s: MOD0 relative offset outside image\n", m->module.name);
    return displacement < 0 ? origin - magnitude : origin + magnitude;
}
static uint64_t add_signed(uint64_t value, int64_t addend, const char *name) {
    uint64_t magnitude = addend < 0 ? (uint64_t)(-(addend + 1)) + 1 : (uint64_t)addend;
    if ((addend < 0 && magnitude > value) || (addend >= 0 && magnitude > UINT64_MAX - value))
        FATAL("%s: relocation value overflow\n", name);
    return addend < 0 ? value - magnitude : value + magnitude;
}
static uint64_t code_address(const LoadedModule *m, uint64_t off) {
    if ((off & 3) || off < m->module.text_off || off - m->module.text_off >= m->module.text_size ||
        !(shared_native_manifest_current() ?
            native_manifest_is_code_address(shared_native_manifest_current(), m->module.base + off) :
            dispatch_lookup(m->module.base + off) != NULL))
        FATAL("%s: initializer outside admitted code\n", m->module.name);
    return m->module.base + off;
}
static const char *symbol_name(const LoadedModule *m, const Elf64Sym *symbol) {
    if (symbol->name >= m->string_size || !memchr(m->strings + symbol->name, 0, m->string_size - symbol->name))
        FATAL("%s: unterminated/out-of-range symbol name\n", m->module.name);
    return m->strings + symbol->name;
}
static uint64_t defined_address(const LoadedModule *m, const Elf64Sym *symbol) {
    if (!symbol->shndx || symbol->shndx >= 0xff00 || (symbol->info & 15) == 6 || (symbol->info & 15) == 10)
        FATAL("%s: unsupported special/TLS/IFUNC symbol definition\n", m->module.name);
    if (!in_image(m, symbol->value, symbol->size) ||
        (!symbol->size && symbol->value > m->module.size)) FATAL("%s: symbol outside module\n", m->module.name);
    return m->module.base + symbol->value;
}

static uint64_t relr_advance(const LoadedModule *m, uint64_t cursor, uint64_t bytes) {
    if (bytes > UINT64_MAX - cursor) FATAL("%s: RELR address cursor overflow\n", m->module.name);
    return cursor + bytes;
}
static void relr_write(const LoadedModule *m, uint64_t off, RelrWrite *writes,
                       size_t *count, size_t limit) {
    bool in_bss = off >= m->image.bss_offset && m->image.bss_size >= 8 &&
        off - m->image.bss_offset <= m->image.bss_size - 8;
    if ((off & 7) || !in_image(m, off, 8) || (backed_tail(m, off) < 8 && !in_bss))
        FATAL("%s: unaligned/out-of-image RELR destination 0x%llx\n", m->module.name,
            (unsigned long long)off);
    if (*count >= limit) FATAL("%s: RELR decoded relocation count exceeds limit\n", m->module.name);
    /* Always read the original owned image, before ANY RELA/RELR write. The NSO
     * link-time address origin is zero, so its relocation factor is module.base. */
    uint64_t addend = read64(m->image.bytes + off);
    if (addend > UINT64_MAX - m->module.base)
        FATAL("%s: RELR implicit addend overflow\n", m->module.name);
    if (writes) writes[*count] = (RelrWrite){off, m->module.base + addend};
    ++*count;
}
/* ELF gABI 6.2: an address anchors 63-word bitmap blocks. This is a two-pass
 * decoder over immutable image bytes: first count/validate, then freeze writes.
 * https://gabi.xinuos.com/elf/06-reloc.html#relative-relocation-table */
static size_t decode_relr(const LoadedModule *m, const uint8_t *table, uint64_t size,
                          RelrWrite *writes, size_t limit) {
    uint64_t cursor = 0;
    bool anchored = false;
    size_t count = 0;
    for (uint64_t i = 0; i < size; i += 8) {
        uint64_t word = read64(table + i);
        if (!(word & 1)) {
            cursor = relr_advance(m, word, 8);
            relr_write(m, word, writes, &count, limit);
            anchored = true;
        } else {
            if (!anchored) FATAL("%s: RELR bitmap before address\n", m->module.name);
            uint64_t next = relr_advance(m, cursor, 63 * 8);
            for (unsigned bit = 0; bit < 63; ++bit)
                if ((word >> (bit + 1)) & 1)
                    relr_write(m, cursor + bit * 8, writes, &count, limit);
            cursor = next;
        }
    }
    return count;
}
static int compare_relr(const void *a, const void *b) {
    uint64_t left = ((const RelrWrite *)a)->off, right = ((const RelrWrite *)b)->off;
    return (left > right) - (left < right);
}
static void validate_relr_overlap(const LoadedModule *m) {
    for (size_t i = 1; i < m->relr_count; ++i)
        if (m->relr_writes[i].off == m->relr_writes[i - 1].off)
            FATAL("%s: duplicate RELR destination\n", m->module.name);
    for (size_t i = 0; i < m->relocation_count; ++i) {
        const Elf64Rela *r = &m->relocations[i];
        if ((uint32_t)r->info == R_NONE) continue;
        size_t lo = 0, hi = m->relr_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (m->relr_writes[mid].off < r->off) lo = mid + 1;
            else hi = mid;
        }
        /* Difference comparisons also cover partially overlapping unaligned
         * RELA writes, without overflowing either destination endpoint. */
        if ((lo < m->relr_count && m->relr_writes[lo].off - r->off < 8) ||
            (lo && r->off - m->relr_writes[lo - 1].off < 8))
            FATAL("%s: overlapping RELR/RELA relocation destinations\n", m->module.name);
    }
}

/* GNU ELF64 hash: four u32 header fields, u64 bloom words, u32 buckets,
 * then contiguous u32 chains indexed from symoffset. We only infer the symbol
 * count; symbol resolution still uses the immutable, fully validated dynsym.
 * Format reference: llvm/include/llvm/Object/ELFTypes.h (Elf_GnuHash_Impl). */
static uint64_t gnu_hash_symbol_count(const LoadedModule *m, uint64_t off,
                                      uint64_t symtab, uint64_t strtab) {
    if ((off | symtab) & 7) FATAL("%s: unaligned GNU hash/dynsym metadata\n", m->module.name);
    const uint8_t *hash = metadata(m, off, 16);
    uint64_t buckets = read32(hash), first = read32(hash + 4), bloom = read32(hash + 8);
    uint32_t shift = read32(hash + 12);
    if (!bloom || (bloom & (bloom - 1)) || shift >= 32 ||
        bloom > MAX_GNU_HASH_WORDS || buckets > MAX_GNU_HASH_WORDS)
        FATAL("%s: invalid/oversized GNU hash header\n", m->module.name);
    uint64_t symbol_bytes = backed_tail(m, symtab);
    if (strtab > symtab && strtab - symtab < symbol_bytes) symbol_bytes = strtab - symtab;
    if (off > symtab && off - symtab < symbol_bytes) symbol_bytes = off - symtab;
    uint64_t capacity = symbol_bytes / sizeof(Elf64Sym);
    if (capacity > MAX_GNU_HASH_WORDS) capacity = MAX_GNU_HASH_WORDS;
    if (!first || first > capacity) FATAL("%s: GNU hash symoffset outside dynsym capacity\n", m->module.name);
    /* u32 dimensions are promoted before multiplication, then checked against
     * initialized backing BEFORE pointer formation or bucket/chain access. */
    uint64_t prefix = 16 + bloom * 8 + buckets * 4;
    uint64_t tail = backed_tail(m, off);
    if (symtab > off && symtab - off < tail) tail = symtab - off;
    if (strtab > off && strtab - off < tail) tail = strtab - off;
    if (prefix > tail) FATAL("%s: GNU hash bloom/buckets outside initialized metadata\n", m->module.name);
    metadata(m, off, prefix);
    const uint8_t *bucket_table = hash + 16 + bloom * 8;
    uint64_t last = 0;
    for (uint64_t i = 0; i < buckets; ++i) {
        uint64_t index = read32(bucket_table + i * 4);
        if (!index) continue;
        if (index < first || index >= capacity)
            FATAL("%s: GNU hash bucket outside dynsym capacity\n", m->module.name);
        if (index > last) last = index;
    }
    /* Empty buckets describe only the unindexed prefix; never read a chain
     * before symoffset. This also admits a zero-bucket empty GNU table. */
    if (!last) return first;
    uint64_t chains = (tail - prefix) / 4;
    if (chains > capacity - first) chains = capacity - first;
    uint64_t end = last - first;
    const uint8_t *chain_table = hash + prefix;
    while (end < chains && !(read32(chain_table + end * 4) & 1)) ++end;
    if (end >= chains)
        FATAL("%s: GNU hash chain has no bounded terminator\n", m->module.name);
    /* Validate every bucket, without quadratic per-bucket chain walks. Earlier
     * chains must end before another bucket starts; the final chain is bounded
     * above. All inspected preceding words lie within that validated range. */
    for (uint64_t i = 0; i < buckets; ++i) {
        uint64_t index = read32(bucket_table + i * 4);
        if (index && index != first && !(read32(chain_table + (index - first - 1) * 4) & 1))
            FATAL("%s: GNU hash bucket starts inside a chain\n", m->module.name);
    }
    return first + end + 1;
}

typedef struct { uint64_t value; bool seen; } DynamicValue;
static void parse_metadata(LoadedModule *m) {
    Module *module = &m->module;
    const uint8_t *start = metadata(m, 0, 8);
    uint64_t mod = read32(start + 4);
    const uint8_t *mod0 = metadata(m, mod, 0x1c);
    if ((mod & 3) || memcmp(mod0, "MOD0", 4)) FATAL("%s: invalid MOD0\n", module->name);
    uint64_t dynamic = relative_offset(m, mod, read_signed32(mod0 + 4));
    uint64_t bss_start = relative_offset(m, mod, read_signed32(mod0 + 8));
    uint64_t bss_end = relative_offset(m, mod, read_signed32(mod0 + 12));
    if (bss_start > bss_end || (bss_end > bss_start &&
        (bss_start < m->image.bss_offset || bss_end - m->image.bss_offset > m->image.bss_size)))
        FATAL("%s: MOD0 BSS outside decoded BSS\n", module->name);
    if (dynamic & 7) FATAL("%s: unaligned dynamic table\n", module->name);
    uint64_t entries = backed_tail(m, dynamic) / 16;
    DynamicValue tags[38] = {{0}};
    DynamicValue gnu_hash = {0};
    bool terminated = false;
    uint64_t used = 0;
    for (; used < entries; ++used) {
        const uint8_t *entry = metadata(m, dynamic + used * 16, 16);
        uint64_t tag = read64(entry), value = read64(entry + 8);
        if (tag == DT_NULL) { terminated = true; break; }
        if (tag == DT_REL || tag == DT_RELSZ || tag == DT_RELENT ||
            (tag >= UINT64_C(0x6000000f) && tag <= UINT64_C(0x60000012)) ||
            (tag >= UINT64_C(0x6fffe000) && tag <= UINT64_C(0x6fffe003)))
            FATAL("%s: unsupported REL/RELR/packed dynamic relocation format "
                "(tag=0x%llx value=0x%llx entry=0x%llx)\n", module->name,
                (unsigned long long)tag, (unsigned long long)value,
                (unsigned long long)(dynamic + used * 16));
        if (tag < 38 && tag != DT_NEEDED) {
            if (tags[tag].seen && tags[tag].value != value) FATAL("%s: conflicting dynamic tag %llu\n", module->name, (unsigned long long)tag);
            tags[tag] = (DynamicValue){value, true};
        }
        if (tag == DT_GNU_HASH) {
            if (gnu_hash.seen && gnu_hash.value != value)
                FATAL("%s: conflicting DT_GNU_HASH dynamic tag\n", module->name);
            gnu_hash = (DynamicValue){value, true};
        }
        /* Unconsumed platform/version/flags tags are metadata, not relocation types. */
    }
    if (!terminated) FATAL("%s: dynamic table has no bounded DT_NULL\n", module->name);
    if ((!tags[DT_HASH].seen && !gnu_hash.seen) || !tags[DT_SYMTAB].seen || !tags[DT_STRTAB].seen || !tags[DT_STRSZ].seen ||
        !tags[DT_SYMENT].seen || tags[DT_SYMENT].value != sizeof(Elf64Sym))
        FATAL("%s: missing/unsupported dynsym metadata (SysV/GNU hash required)\n", module->name);
    uint64_t symbols;
    if (tags[DT_HASH].seen) {
        /* Preserve the established SysV count/validation when both are present.
         * A GNU table is used for count inference only when DT_HASH is absent. */
        const uint8_t *hash = metadata(m, tags[DT_HASH].value, 8);
        uint64_t buckets = read32(hash);
        symbols = read32(hash + 4);
        if (!symbols || symbols > SIZE_MAX / sizeof(Elf64Sym)) FATAL("%s: invalid dynsym count\n", module->name);
        metadata(m, tags[DT_HASH].value, 8 + (buckets + symbols) * 4);
    } else {
        symbols = gnu_hash_symbol_count(m, gnu_hash.value, tags[DT_SYMTAB].value, tags[DT_STRTAB].value);
    }
    if (!symbols || symbols > SIZE_MAX / sizeof(Elf64Sym)) FATAL("%s: invalid dynsym count\n", module->name);
    const uint8_t *symtab = metadata(m, tags[DT_SYMTAB].value, symbols * sizeof(Elf64Sym));
    uint64_t strings = tags[DT_STRSZ].value;
    const uint8_t *strtab = metadata(m, tags[DT_STRTAB].value, strings);
    if (strings > SIZE_MAX || strtab[0] || strtab[strings - 1]) FATAL("%s: invalid dynamic string table\n", module->name);
    m->symbols = malloc((size_t)symbols * sizeof(Elf64Sym)); m->strings = malloc((size_t)strings);
    if (!m->symbols || !m->strings) FATAL("%s: allocating immutable symbol metadata failed\n", module->name);
    memcpy(m->symbols, symtab, (size_t)symbols * sizeof(Elf64Sym)); memcpy(m->strings, strtab, (size_t)strings);
    m->string_size = (size_t)strings;
    module->dynamic = module->base + dynamic;
    module->symtab = module->base + tags[DT_SYMTAB].value;
    module->strtab = module->base + tags[DT_STRTAB].value; module->nsyms = symbols;
    for (uint64_t i = 0; i < symbols; ++i) {
        const Elf64Sym *symbol = &m->symbols[i];
        symbol_name(m, symbol);
        if (symbol->shndx && symbol->shndx < 0xff00 && !in_image(m, symbol->value, symbol->size))
            FATAL("%s: defined symbol range outside image\n", module->name);
    }
    for (uint64_t i = 0; i < used; ++i) {
        const uint8_t *entry = metadata(m, dynamic + i * 16, 16);
        if (read64(entry) == DT_NEEDED) {
            uint64_t offset = read64(entry + 8);
            if (offset >= strings || !memchr(m->strings + offset, 0, (size_t)(strings - offset)))
                FATAL("%s: invalid DT_NEEDED string\n", module->name);
        }
    }
    if (tags[DT_INIT].seen) module->init = code_address(m, tags[DT_INIT].value);
    if (tags[DT_FINI].seen) module->fini = code_address(m, tags[DT_FINI].value);
    const unsigned array_tags[2] = {DT_INIT_ARRAY, DT_FINI_ARRAY}, size_tags[2] = {DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ};
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t size = tags[size_tags[i]].value, off = tags[array_tags[i]].value;
        if (size && (!tags[array_tags[i]].seen || (size & 7) || (off & 7))) FATAL("%s: invalid init/fini array\n", module->name);
        if (size) metadata(m, off, size);
        if (!i) { module->init_array = size ? module->base + off : 0; module->init_array_sz = size; }
        else { module->fini_array = size ? module->base + off : 0; module->fini_array_sz = size; }
    }
    uint64_t sizes[2] = {tags[DT_RELASZ].value, tags[DT_PLTRELSZ].value};
    uint64_t offsets[2] = {tags[DT_RELA].value, tags[DT_JMPREL].value};
    if ((sizes[0] && (!tags[DT_RELA].seen || !tags[DT_RELAENT].seen || tags[DT_RELAENT].value != sizeof(Elf64Rela))) ||
        (sizes[1] && (!tags[DT_JMPREL].seen || !tags[DT_PLTREL].seen || tags[DT_PLTREL].value != DT_RELA)))
        FATAL("%s: unsupported/missing RELA metadata\n", module->name);
    if ((sizes[0] % sizeof(Elf64Rela)) || (sizes[1] % sizeof(Elf64Rela)) || sizes[0] > SIZE_MAX - sizes[1])
        FATAL("%s: invalid RELA table sizes\n", module->name);
    for (unsigned i = 0; i < 2; ++i) if (sizes[i]) metadata(m, offsets[i], sizes[i]);
    if (sizes[0] && sizes[1] && offsets[0] < offsets[1] + sizes[1] && offsets[1] < offsets[0] + sizes[0])
        FATAL("%s: overlapping RELA tables\n", module->name);
    m->relocation_count = (size_t)((sizes[0] + sizes[1]) / sizeof(Elf64Rela));
    if (m->relocation_count) {
        m->relocations = malloc(m->relocation_count * sizeof(Elf64Rela));
        m->relocation_values = calloc(m->relocation_count, sizeof(uint64_t));
        if (!m->relocations || !m->relocation_values) FATAL("%s: relocation metadata allocation failed\n", module->name);
        if (sizes[0]) memcpy(m->relocations, m->image.bytes + offsets[0], (size_t)sizes[0]);
        if (sizes[1]) memcpy((uint8_t *)m->relocations + sizes[0], m->image.bytes + offsets[1], (size_t)sizes[1]);
    }
    if (tags[DT_RELR].seen || tags[DT_RELRSZ].seen || tags[DT_RELRENT].seen) {
        uint64_t off = tags[DT_RELR].value, size = tags[DT_RELRSZ].value;
        if (!tags[DT_RELR].seen || !tags[DT_RELRSZ].seen || !tags[DT_RELRENT].seen ||
            tags[DT_RELRENT].value != 8 || (off & 7) || (size & 7))
            FATAL("%s: invalid/missing ELF64 RELR metadata\n", module->name);
        size_t limit = m->image.size / 8;
        if (limit > MAX_RELR_WRITES) limit = MAX_RELR_WRITES;
        if (size / 8 > limit) FATAL("%s: RELR table entry count exceeds limit\n", module->name);
        if (size) {
            const uint8_t *table = metadata(m, off, size);
            for (unsigned i = 0; i < 2; ++i)
                if (sizes[i] && off < offsets[i] + sizes[i] && offsets[i] < off + size)
                    FATAL("%s: overlapping RELR/RELA tables\n", module->name);
            m->relr_count = decode_relr(m, table, size, NULL, limit);
            if (m->relr_count) {
                m->relr_writes = malloc(m->relr_count * sizeof *m->relr_writes);
                if (!m->relr_writes) FATAL("%s: RELR write allocation failed\n", module->name);
                decode_relr(m, table, size, m->relr_writes, m->relr_count);
                qsort(m->relr_writes, m->relr_count, sizeof *m->relr_writes, compare_relr);
                validate_relr_overlap(m);
            }
        }
    }
}

uint64_t loader_find_symbol(const char *name) {
    if (!name) return 0;
    for (unsigned mi = 0; mi < loaded_count; ++mi) {
        const LoadedModule *m = &loaded[mi];
        for (uint64_t i = 1; i < m->module.nsyms; ++i)
            if (m->symbols[i].shndx && !strcmp(symbol_name(m, &m->symbols[i]), name))
                return defined_address(m, &m->symbols[i]);
    }
    return 0;
}
const char *loader_symbol_at(uint64_t address, uint64_t *offset) {
    const char *best = NULL; uint64_t best_address = 0;
    if (offset) *offset = 0;
    for (unsigned mi = 0; mi < loaded_count; ++mi) {
        const LoadedModule *m = &loaded[mi];
        if (address < m->module.base || address - m->module.base >= m->module.size) continue;
        for (uint64_t i = 1; i < m->module.nsyms; ++i) {
            const Elf64Sym *symbol = &m->symbols[i];
            if (!symbol->shndx || (symbol->info & 15) != 2) continue;
            uint64_t found = defined_address(m, symbol);
            if (found <= address && (!best || found > best_address)) { best = symbol_name(m, symbol); best_address = found; }
        }
    }
    if (best && offset) *offset = address - best_address;
    return best;
}
static uint64_t resolve(const LoadedModule *m, uint32_t index) {
    if (index >= m->module.nsyms) FATAL("%s: relocation symbol index out of range\n", m->module.name);
    const Elf64Sym *symbol = &m->symbols[index];
    if (symbol->shndx) return defined_address(m, symbol);
    const char *name = symbol_name(m, symbol);
    HostFn host = hle_lookup_impl(name);
    if (host) return hle_register(name, host);
    uint64_t found = loader_find_symbol(name);
    if (found) return found;
    if ((symbol->info >> 4) == 2) return 0;
    FATAL("%s: unresolved strong import %s\n", m->module.name, name);
}
static void plan_relocations(LoadedModule *m) {
    for (size_t i = 0; i < m->relocation_count; ++i) {
        const Elf64Rela *r = &m->relocations[i];
        uint32_t type = (uint32_t)r->info, symbol = (uint32_t)(r->info >> 32);
        if (type == R_NONE) continue; /* explicitly supported no-op */
        if (!in_image(m, r->off, 8)) FATAL("%s: relocation destination outside module\n", m->module.name);
        switch (type) {
        case R_RELATIVE:
            if (symbol || r->addend < 0 || (uint64_t)r->addend > m->module.size)
                FATAL("%s: relative relocation is not a module offset\n", m->module.name);
            m->relocation_values[i] = add_signed(m->module.base, r->addend, m->module.name); break;
        case R_ABS64: case R_GLOB_DAT: case R_JUMP_SLOT: {
            uint64_t value = resolve(m, symbol);
            m->relocation_values[i] = value ? add_signed(value, r->addend, m->module.name) : 0;
            break;
        }
        default: FATAL("%s: unsupported AArch64 relocation type %u\n", m->module.name, type);
        }
    }
}
static bool generated_target(uint64_t address) {
    const NativeManifest *native = shared_native_manifest_current();
    return address && (native ? native_manifest_is_code_address(native, address) : dispatch_lookup(address) != NULL);
}
static void validate_arrays(const LoadedModule *m) {
    const uint64_t arrays[2] = {m->module.init_array, m->module.fini_array};
    const uint64_t sizes[2] = {m->module.init_array_sz, m->module.fini_array_sz};
    for (unsigned a = 0; a < 2; ++a) for (uint64_t i = 0; i < sizes[a]; i += 8) {
        uint64_t target = read64(m->image.bytes + arrays[a] - m->module.base + i);
        if (target && !generated_target(target)) FATAL("%s: init/fini array target outside admitted code\n", m->module.name);
    }
}
static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) FATAL("cannot open NSO %s\n", path);
    if (fseek(file, 0, SEEK_END)) FATAL("cannot seek NSO %s\n", path);
    long end = ftell(file);
    if (end < NSO_IMAGE_HEADER_SIZE || (uint64_t)end > SIZE_MAX || fseek(file, 0, SEEK_SET))
        FATAL("invalid NSO file length %s\n", path);
    *size = (size_t)end; uint8_t *raw = malloc(*size);
    if (!raw || fread(raw, 1, *size, file) != *size || ferror(file)) FATAL("cannot read complete NSO %s\n", path);
    if (fclose(file)) FATAL("cannot close NSO %s\n", path);
    return raw;
}
void loader_load(const char *exefs) {
    if (started || !exefs || !*exefs) FATAL("NSO loading requires one fresh selected session\n");
    started = true;
    const TitleManifest *manifest = shared_manifest_current();
    const NativeManifest *native = shared_native_manifest_current();
    const TitleAdapter *adapter = title_adapter_selected();
    const ModuleAbiDescriptor *descriptor = shared_gamelib_descriptor();
    ModuleAbiFailure failure;
    if (!adapter || (native ?
        (descriptor || native->title_id != (uint32_t)adapter->id || native->module_count != adapter->module_count) :
        (!manifest || manifest->title_id != (uint32_t)adapter->id ||
         manifest->profile.module_count != adapter->module_count ||
         module_abi_validate(descriptor, &manifest->profile, &failure) != MODULE_ABI_OK)))
        FATAL("NSO loader has no matching validated manifest/adapter/descriptor\n");
    unsigned count = adapter->module_count;
    for (unsigned i = 0; i < count; ++i) {
        const ModuleAbiModuleSpec *spec = native ? NULL : &manifest->profile.modules[i];
        const NativeModuleSpec *original = native ? &native->modules[i] : NULL;
        const TitleModulePolicy *policy = &adapter->modules[i];
        if (!policy->name || policy->module_id != (native ? original->module_id : spec->module_id) ||
            policy->canonical_base != (native ? original->canonical_base : spec->canonical_base))
            FATAL("NSO loader module order differs from selected adapter\n");
        char path[4096]; int length = snprintf(path, sizeof path, "%s/%s", exefs, policy->name);
        if (length < 0 || (size_t)length >= sizeof path) FATAL("NSO path too long\n");
        size_t size; uint8_t *raw = read_file(path, &size); uint8_t digest[32];
        if (!shared_sha256_bytes(raw, size, digest) ||
            memcmp(digest, native ? original->nso_sha256 : spec->nso_sha256, sizeof digest))
            FATAL("%s: complete original NSO SHA256 mismatch\n", policy->name);
        LoadedModule *m = &loaded[i]; NsoImageFailure decode;
        enum NsoImageStatus status = native ?
            nso_image_decode_native(raw, size, original, &native->identity, native->mapping_alignment, &m->image, &decode) :
            nso_image_decode(raw, size, spec, &manifest->profile.identity, 16384, &m->image, &decode);
        free(raw);
        if (status != NSO_IMAGE_OK) FATAL("%s: NSO decode rejected: %s (segment %u)\n", policy->name,
            nso_image_status_string(status), decode.segment_index);
        /* The kernel's permission regions cover text->rodata->data including
         * alignment gaps. Reject another ordering before guest writes, so its
         * unsigned range differences cannot underflow. */
        if (m->image.segments[0].memory_offset > m->image.segments[1].memory_offset ||
            m->image.segments[1].memory_offset > m->image.segments[2].memory_offset)
            FATAL("%s: unsupported NSO segment order\n", policy->name);
        m->module = (Module){.name = policy->name, .base = m->image.actual_base, .size = m->image.size,
            .text_off = m->image.segments[0].memory_offset, .text_size = m->image.segments[0].size,
            .ro_off = m->image.segments[1].memory_offset, .ro_size = m->image.segments[1].size,
            .data_off = m->image.segments[2].memory_offset, .data_size = m->image.data_size_with_bss};
        parse_metadata(m);
    }
    loaded_count = count; /* every module is present before cross-module resolution */
    for (unsigned i = 0; i < count; ++i) plan_relocations(&loaded[i]);
    for (unsigned i = 0; i < count; ++i) {
        LoadedModule *m = &loaded[i];
        for (size_t r = 0; r < m->relocation_count; ++r)
            if ((uint32_t)m->relocations[r].info != R_NONE)
                memcpy(m->image.bytes + m->relocations[r].off, &m->relocation_values[r], sizeof(uint64_t));
        for (size_t r = 0; r < m->relr_count; ++r)
            memcpy(m->image.bytes + m->relr_writes[r].off, &m->relr_writes[r].value, sizeof(uint64_t));
        validate_arrays(m);
    }
    /* Only now may any decoded bytes reach the already-reserved guest window. */
    for (unsigned i = 0; i < count; ++i) {
        LoadedModule *m = &loaded[i];
        plat_commit(m->module.base, m->module.size);
        memcpy((void *)(uintptr_t)m->module.base, m->image.bytes, m->image.size);
        g_modules[i] = m->module;
        LOG("loaded %-7s at %llx size %llx syms %llu (%zu checked RELA entries, %zu RELR writes)\n", m->module.name,
            (unsigned long long)m->module.base, (unsigned long long)m->module.size,
            (unsigned long long)m->module.nsyms, m->relocation_count, m->relr_count);
        nso_image_destroy(&m->image);
        free(m->relocations); free(m->relocation_values); m->relocations = NULL; m->relocation_values = NULL;
        free(m->relr_writes); m->relr_writes = NULL;
    }
    g_nmodules = (int)count;
}

/* Native internal SDK branches must reach the same host implementations as
 * undefined imports. Registered only for the explicitly selected backend. */
void loader_native_host_definitions(void) {
    if(!aot_dispatch_execution_backend())FATAL("native definitions require selected execution backend\n");
    unsigned registered=0;
    for(unsigned i=0;i<loaded_count;i++) {
        LoadedModule *m=&loaded[i];
        for(uint64_t s=1;s<m->module.nsyms;s++) {
            const Elf64Sym *symbol=&m->symbols[s];
            if(!symbol->shndx || symbol->shndx>=0xff00 || (symbol->info&15)!=2)continue;
            uint64_t address=m->module.base+symbol->value;
            if(!aot_dispatch_contains_text(address))continue;
            const char *name=symbol_name(m,symbol);
            HostFn fn=hle_lookup_impl(name);
            /* These CPU3 helpers are intentionally outside HLE_FUNC sections;
             * generated code uses them directly for the same SDK definitions. */
            if(!fn) {
                static const struct {const char *name;HostFn function;} memory[] = {
                    {"memcpy",hle_memcpy},{"memmove",hle_memmove},{"memset",hle_memset},
                    {"memcmp",hle_memcmp},{"bcmp",hle_bcmp},{"strlen",hle_strlen},
                    {"strcmp",hle_strcmp},{"strncmp",hle_strncmp},{"memchr",hle_memchr}
                };
                for(size_t k=0;k<sizeof memory/sizeof memory[0];k++)
                    if(!strcmp(name,memory[k].name)){fn=memory[k].function;break;}
            }
            if(!fn)continue;
            if(aot_dispatch_override(address,fn)!=AOT_OK)FATAL("cannot bind native definition %s\n",name);
            registered++;
        }
    }
    LOG("[native-engine] %u original definition host bindings\n",registered);
}
