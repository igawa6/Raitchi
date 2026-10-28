#include "shared_gpu_cache.h"
#include "common_session.h"
#include "gpu_cache_build.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool unhex(const char *text, uint8_t *out, size_t size) {
    if (strlen(text) != size * 2) return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; ++j) {
            unsigned char c = (unsigned char)text[2 * i + j];
            if (c >= '0' && c <= '9') value = value * 16 + c - '0';
            else if (c >= 'a' && c <= 'f') value = value * 16 + c - 'a' + 10;
            else return false;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}
bool shared_gpu_build_identity(bool compiler, uint8_t out[32]) {
    return GPU_CACHE_BUILD_READY && out && unhex(compiler ? GPU_COMPILER_ID_HEX : GPU_RENDERER_ID_HEX, out, 32);
}
bool shared_gpu_environment(uint8_t out[32]) {
    extern char **environ;
    uint8_t hashes[256][32];
    const char *names[256]; size_t name_lengths[256];
    size_t n = 0, hashed = 0;
    for (char **e = environ; *e; ++e) {
        if (strncmp(*e, "LARECOMP_", 9) && strncmp(*e, "TU_", 3) &&
            strncmp(*e, "MESA_", 5) && strncmp(*e, "VK_", 3)) continue;
        const char *equal = strchr(*e, '=');
        if (!equal || n == 256) return false;
        size_t length = (size_t)(equal - *e);
        // getenv chooses the first duplicate name; a sorted multiset would hide
        // a changed winner. Refuse ambiguous environments rather than replay.
        for (size_t i = 0; i < n; ++i)
            if (length == name_lengths[i] && !memcmp(names[i], *e, length)) return false;
        names[n] = *e; name_lengths[n] = length;
        ++n;
        /* These opt-in flags only add bounded diagnostic output/metadata. Keep
           counting and duplicate-checking them above, but don't make a toggle
           invalidate otherwise compatible shader preparation. */
        if ((length == sizeof "LARECOMP_SAMPLING_TRACE" - 1 &&
             !memcmp(*e, "LARECOMP_SAMPLING_TRACE", length)) ||
            (length == sizeof "LARECOMP_LOG_COMMAND_POOLS" - 1 &&
             !memcmp(*e, "LARECOMP_LOG_COMMAND_POOLS", length)) ||
            (length == sizeof "LARECOMP_FONT_TRACE" - 1 &&
             !memcmp(*e, "LARECOMP_FONT_TRACE", length))) continue;
        if (!shared_sha256_bytes(*e, strlen(*e) + 1, hashes[hashed++])) return false;
    }
    for (size_t i = 1; i < hashed; ++i) {
        uint8_t item[32]; memcpy(item, hashes[i], 32);
        size_t j = i;
        while (j && memcmp(hashes[j - 1], item, 32) > 0) {
            memcpy(hashes[j], hashes[j - 1], 32); --j;
        }
        memcpy(hashes[j], item, 32);
    }
    return out && shared_sha256_bytes(hashes, hashed * 32, out);
}
bool shared_gpu_game_identity(uint8_t out[32]) {
    const NativeManifest *native = shared_native_manifest_current();
    const TitleManifest *aot = shared_manifest_current();
    if (!native && !aot) return false;
    /* Only verified immutable original-module identity and guest layout. AOT code,
       runtime contract and process addresses are deliberately not game identity. */
    struct {
        uint64_t program;
        uint32_t title, count;
        struct {
            uint32_t id, reserved;
            uint8_t build[32], sha[32];
            uint64_t base, mapped, text_offset, text_size;
        } modules[MODULE_ABI_MAX_MODULES];
    } in;
    memset(&in, 0, sizeof in);
    in.program = native ? native->program_id : 0;
    in.title = native ? native->title_id : aot->title_id;
    in.count = native ? native->module_count : aot->profile.module_count;
    if (!in.title || !in.count || in.count > MODULE_ABI_MAX_MODULES) return false;
    for (unsigned i = 0; i < in.count; ++i) {
        if (native) {
            const NativeModuleSpec *m = &native->modules[i];
            in.modules[i].id = m->module_id;
            memcpy(in.modules[i].build, m->nso_build_id, 32);
            memcpy(in.modules[i].sha, m->nso_sha256, 32);
            in.modules[i].base = m->canonical_base; in.modules[i].mapped = m->mapped_size;
            in.modules[i].text_offset = m->text_offset; in.modules[i].text_size = m->text_size;
        } else {
            const ModuleAbiModuleSpec *m = &aot->modules[i];
            in.modules[i].id = m->module_id;
            memcpy(in.modules[i].build, m->nso_build_id, 32);
            memcpy(in.modules[i].sha, m->nso_sha256, 32);
            in.modules[i].base = m->canonical_base; in.modules[i].mapped = m->mapped_size;
            in.modules[i].text_offset = m->text_offset; in.modules[i].text_size = m->text_size;
        }
    }
    return shared_sha256_bytes(&in, sizeof in, out);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
bool shared_gpu_cache_header(const void *data, size_t length, const SharedGpuDevice *d) {
    const uint8_t *p = data;
    return p && d && length >= 32 && le32(p) == 32 && le32(p + 4) == 1 &&
        le32(p + 8) == d->vendor && le32(p + 12) == d->device && !memcmp(p + 16, d->uuid, 16);
}
bool shared_gpu_cache_metadata(SharedGpuCacheMeta *out, const void *data, size_t length, const SharedGpuDevice *d) {
    if (!out || !shared_gpu_cache_header(data, length, d)) return false;
    memset(out, 0, sizeof *out);
    out->magic = 0x32435047; /* GPC2: opaque blob integrity + device schema, no renderer identity. */
    out->version = 2;
    const uint32_t endian = 1;
    out->abi = (uint32_t)sizeof(void *) | (*(const uint8_t *)&endian ? 0x100u : 0x200u);
    out->api = d->api; out->vendor = d->vendor; out->device = d->device; out->driver = d->driver;
    memcpy(out->uuid, d->uuid, 16); memcpy(out->environment, d->environment, 32);
    out->length = length;
    return shared_sha256_bytes(data, length, out->sha256);
}
bool shared_gpu_cache_tag_write(const char *path, const SharedGpuCacheMeta *metadata) {
    char temporary[4096];
    if (!path || !metadata || snprintf(temporary, sizeof temporary, "%s.tmp", path) >= (int)sizeof temporary) return false;
    FILE *f = fopen(temporary, "wb");
    if (!f) return false;
    bool ok = fwrite(metadata, sizeof *metadata, 1, f) == 1;
    if (fclose(f)) ok = false;
    if (!ok || rename(temporary, path)) { remove(temporary); return false; }
    return true;
}
static bool legacy_tag(const char *tag, size_t length, const char *gpu) {
    /* Import only the shipped old schema. The translator prefix was unrelated to
       opaque VkPipelineCache validity; retain its exact GPU/driver/UUID tail. */
    const char prefix[] = "beta_4 translator ";
    if (!gpu || length != sizeof prefix - 1 + 12 + 11 + strlen(gpu) ||
        memcmp(tag, prefix, sizeof prefix - 1)) return false;
    size_t at = sizeof prefix - 1;
    uint8_t ignored[6]; char translator[13]; memcpy(translator, tag + at, 12); translator[12] = 0;
    if (!unhex(translator, ignored, 6)) return false;
    at += 12;
    return !memcmp(tag + at, " cache 1 | ", 11) && !memcmp(tag + at + 11, gpu, strlen(gpu));
}
void *shared_gpu_cache_load(const char *path, const char *tag_path, const SharedGpuDevice *d,
                           const char *legacy_gpu, size_t *length, bool *migrated) {
    *length = 0; *migrated = false;
    uint8_t tag[1024]; size_t count = 0;
    FILE *f = fopen(tag_path, "rb");
    if (f) { count = fread(tag, 1, sizeof tag, f); if (ferror(f) || !feof(f)) count = 0; if (fclose(f)) count = 0; }
    bool legacy = legacy_tag((const char *)tag, count, legacy_gpu);
    if (count != sizeof(SharedGpuCacheMeta) && !legacy) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    bool ok = !fseek(f, 0, SEEK_END);
    long end = ok ? ftell(f) : -1;
    // Inspect fixed header and metadata before any blob-sized allocation. A stale
    // missing/wrong tag or sparse oversized/truncated file must remain cheap.
    uint8_t header[32];
    ok = end >= 32 && (uint64_t)end <= (1ull << 30) && !fseek(f, 0, SEEK_SET) &&
         fread(header, 1, sizeof header, f) == sizeof header;
    SharedGpuCacheMeta expected, stored;
    ok = ok && shared_gpu_cache_metadata(&expected, header, sizeof header, d);
    if (ok && !legacy) {
        memcpy(&stored, tag, sizeof stored);
        ok = !memcmp(&stored, &expected, offsetof(SharedGpuCacheMeta, length)) && stored.length == (uint64_t)end;
    }
    size_t n = ok ? (size_t)end : 0;
    void *data = ok ? malloc(n) : NULL;
    ok = data && !fseek(f, 0, SEEK_SET) && fread(data, 1, n, f) == n;
    if (fclose(f)) ok = false;
    if (!ok) { free(data); return NULL; }
    if (!shared_gpu_cache_metadata(&expected, data, n, d)) { free(data); return NULL; }
    bool valid = count == sizeof expected && !memcmp(tag, &expected, sizeof expected);
    if (!valid && legacy) {
        /* The caller must let Vulkan accept these header-compatible legacy bytes
           before upgrading the sidecar. Legacy did not record env/API/integrity. */
        valid = true;
        *migrated = true;
    }
    if (!valid) { free(data); return NULL; }
    *length = n; return data;
}
