/* Raitchi metadata-only, game/version-scoped shader preparation profiles.
 * Uses the discovery journal's explicit semantic field codec, never raw structs.
 */
#include "shader_profile.h"

static const uint8_t profile_magic[8] = {'N', 'V', 'N', 'P', 'R', 'O', 'F', 0};

bool shader_profile_entry_valid(const ShaderProfileEntry *e) {
    if (!e || !nvn_disc_rec_valid(&e->rec)) return false;
    for (unsigned i = 0; i < 2; i++) {
        uint64_t bytes = (uint64_t)e->code_bytes[i] + e->constant_bytes[i];
        if (bytes > NVN_DISCOVERY_MAX_STAGE_BYTES) return false;
    }
    if (e->rec.kind == 'C')
        return e->code_bytes[0] && !e->code_bytes[1] && !e->constant_bytes[1] &&
               nvn_disc_key_compute(&e->vkey) && nvn_disc_key_zero(&e->fkey);
    if (!nvn_disc_key_valid(&e->vkey) || !nvn_disc_key_valid(&e->fkey) ||
        !nvn_disc_zero(e->vkey.rt_kind, sizeof e->vkey.rt_kind) ||
        !nvn_disc_zero(e->fkey.attr_kind, sizeof e->fkey.attr_kind)) return false;
    if (e->fkey.flip_x != e->vkey.flip_x || e->fkey.flip_y != e->vkey.flip_y ||
        e->fkey.depth_minus_one ||
        e->vkey.flip_x != ((e->rec.flags & SR_FLIP_X) != 0) ||
        e->vkey.flip_y != ((e->rec.flags & SR_FLIP_Y) != 0) ||
        e->vkey.depth_minus_one != ((e->rec.flags & SR_DEPTH_MINUS_ONE) != 0)) return false;
    return e->code_bytes[0] >= 0x80 && (e->rec.fs ? e->code_bytes[1] >= 0x80 :
           (!e->code_bytes[1] && !e->constant_bytes[1]));
}

static bool profile_encode(const ShaderProfileEntry *e, uint8_t out[SHADER_PROFILE_METADATA_BYTES]) {
    if (!shader_profile_entry_valid(e)) return false;
    NvnDiscWriter w = {out, 0, SHADER_PROFILE_METADATA_BYTES, true};
    nvn_disc_write_rec(&w, &e->rec);
    nvn_disc_write_key(&w, &e->vkey);
    nvn_disc_write_key(&w, &e->fkey);
    for (unsigned i = 0; i < 2; i++) {
        nvn_disc_w32(&w, e->code_bytes[i]);
        nvn_disc_w32(&w, e->constant_bytes[i]);
    }
    return w.ok && w.n == SHADER_PROFILE_METADATA_BYTES;
}

static bool profile_decode(const uint8_t *bytes, ShaderProfileEntry *out) {
    memset(out, 0, sizeof *out);
    NvnDiscReader r = {bytes, SHADER_PROFILE_METADATA_BYTES, 0, true};
    nvn_disc_read_rec(&r, &out->rec);
    nvn_disc_read_key(&r, &out->vkey);
    nvn_disc_read_key(&r, &out->fkey);
    for (unsigned i = 0; i < 2; i++) {
        out->code_bytes[i] = nvn_disc_r32(&r);
        out->constant_bytes[i] = nvn_disc_r32(&r);
    }
    return r.ok && r.at == r.n && shader_profile_entry_valid(out);
}

void shader_profile_close(ShaderProfile *p) {
    if (p) { free(p->entries); memset(p, 0, sizeof *p); }
}

typedef struct {
    uint8_t digest[32];
    size_t offset;
    bool used;
} ProfileKnown;
#define PROFILE_KNOWN_SLOTS (NVN_DISCOVERY_MAX_RECORDS * 2u)

static size_t profile_known_slot(const uint8_t digest[32]) {
    return (size_t)(nvn_disc_get64(digest) & (PROFILE_KNOWN_SLOTS - 1));
}

static bool profile_known_find(const ProfileKnown *known, const NvnDiscoveryStore *store,
                               const uint8_t metadata[SHADER_PROFILE_METADATA_BYTES],
                               const uint8_t digest[32]) {
    size_t slot = profile_known_slot(digest);
    for (size_t probes = 0; probes < PROFILE_KNOWN_SLOTS; probes++) {
        const ProfileKnown *k = &known[slot];
        if (!k->used) return false;
        if (!memcmp(k->digest, digest, 32) &&
            !memcmp(store->bytes + k->offset, metadata, SHADER_PROFILE_METADATA_BYTES)) return true;
        slot = (slot + 1) & (PROFILE_KNOWN_SLOTS - 1);
    }
    return false;
}

static bool profile_known_add(ProfileKnown *known, const NvnDiscoveryStore *store, size_t offset) {
    uint8_t digest[32];
    if (offset > store->size || SHADER_PROFILE_METADATA_BYTES > store->size - offset ||
        !nvn_disc_sha(store->sha256, store->bytes + offset, SHADER_PROFILE_METADATA_BYTES, digest)) return false;
    size_t slot = profile_known_slot(digest);
    for (size_t probes = 0; probes < PROFILE_KNOWN_SLOTS; probes++) {
        ProfileKnown *k = &known[slot];
        if (!k->used) {
            memcpy(k->digest, digest, 32); k->offset = offset; k->used = true; return true;
        }
        if (!memcmp(k->digest, digest, 32) &&
            !memcmp(store->bytes + k->offset, store->bytes + offset, SHADER_PROFILE_METADATA_BYTES)) return true;
        slot = (slot + 1) & (PROFILE_KNOWN_SLOTS - 1);
    }
    return false;
}

static ProfileKnown *profile_known_build(const NvnDiscoveryStore *store) {
    if (!store || !store->initialized || !store->sha256 || !store->bytes ||
        store->size < NVN_DISCOVERY_HEADER_BYTES || store->count > NVN_DISCOVERY_MAX_RECORDS) return NULL;
    ProfileKnown *known = calloc(PROFILE_KNOWN_SLOTS, sizeof *known);
    if (!known) return NULL;
    size_t at = NVN_DISCOVERY_HEADER_BYTES;
    for (uint32_t i = 0; i < store->count; i++) {
        if (at > store->size || store->size - at < 4) { free(known); return NULL; }
        size_t n = nvn_disc_get32(store->bytes + at);
        if (n < SHADER_PROFILE_METADATA_BYTES || n > store->size - at - 4 ||
            store->size - at - 4 - n < 32 || !profile_known_add(known, store, at + 4)) {
            free(known); return NULL;
        }
        at += n + 36;
    }
    if (at != store->size) { free(known); return NULL; }
    return known;
}

static bool profile_matches_store(const ShaderProfile *p, const NvnDiscoveryStore *s) {
    return p && s && s->initialized && p->count <= NVN_DISCOVERY_MAX_RECORDS &&
           (!p->count || p->entries) && !memcmp(p->game_id, s->game_id, 32);
}

bool shader_profile_missing_count(const ShaderProfile *profile, const NvnDiscoveryStore *store,
                                  size_t *missing) {
    if (!missing || !profile_matches_store(profile, store)) return false;
    *missing = 0;
    ProfileKnown *known = profile_known_build(store);
    if (!known) return false;
    bool ok = true;
    for (size_t i = 0; i < profile->count; i++) {
        uint8_t metadata[SHADER_PROFILE_METADATA_BYTES], digest[32];
        if (!profile_encode(&profile->entries[i], metadata) ||
            !nvn_disc_sha(store->sha256, metadata, sizeof metadata, digest)) { ok = false; break; }
        if (!profile_known_find(known, store, metadata, digest)) (*missing)++;
    }
    free(known);
    return ok;
}

ShaderProfileResult shader_profile_load_bytes(const void *data, size_t n, const uint8_t game[32],
                                              NvnDiscoverySha256 sha, ShaderProfile *out) {
    if (!out) return SHADER_PROFILE_ERROR;
    memset(out, 0, sizeof *out);
    if (!data || !game || !sha) return SHADER_PROFILE_ERROR;
    if (n > SHADER_PROFILE_MAX_BYTES) return SHADER_PROFILE_LIMIT;
    if (n < SHADER_PROFILE_HEADER_BYTES) return SHADER_PROFILE_CORRUPT;
    const uint8_t *bytes = data;
    uint8_t digest[32];
    ShaderProfileResult result = SHADER_PROFILE_CORRUPT;
    if (memcmp(bytes, profile_magic, 8) ||
        nvn_disc_get32(bytes + 12) != SHADER_PROFILE_HEADER_BYTES ||
        nvn_disc_get32(bytes + 20) != SHADER_PROFILE_RECORD_BYTES) goto done;
    if (nvn_disc_get32(bytes + 8) != SHADER_PROFILE_SCHEMA || memcmp(bytes + 24, game, 32)) {
        result = SHADER_PROFILE_INCOMPATIBLE; goto done;
    }
    uint32_t count = nvn_disc_get32(bytes + 16);
    if (count > NVN_DISCOVERY_MAX_RECORDS) { result = SHADER_PROFILE_LIMIT; goto done; }
    if (n != SHADER_PROFILE_HEADER_BYTES + (uint64_t)count * SHADER_PROFILE_RECORD_BYTES ||
        !nvn_disc_sha(sha, bytes, 88, digest) || memcmp(digest, bytes + 88, 32) ||
        !nvn_disc_sha(sha, bytes + SHADER_PROFILE_HEADER_BYTES, n - SHADER_PROFILE_HEADER_BYTES, digest) ||
        memcmp(digest, bytes + 56, 32)) goto done;
    ShaderProfileEntry *entries = count ? calloc(count, sizeof *entries) : NULL;
    if (count && !entries) { result = SHADER_PROFILE_ERROR; goto done; }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *record = bytes + SHADER_PROFILE_HEADER_BYTES + i * SHADER_PROFILE_RECORD_BYTES;
        if (!nvn_disc_sha(sha, record, SHADER_PROFILE_METADATA_BYTES, digest) ||
            memcmp(digest, record + SHADER_PROFILE_METADATA_BYTES, 32) || !profile_decode(record, &entries[i])) {
            free(entries); goto done;
        }
    }
    memcpy(out->game_id, game, 32); out->entries = entries; out->count = count;
    result = SHADER_PROFILE_LOADED;
done:
    return result;
}

ShaderProfileResult shader_profile_load(const char *path, const uint8_t game[32],
                                        NvnDiscoverySha256 sha, ShaderProfile *out) {
    if (!out) return SHADER_PROFILE_ERROR;
    memset(out, 0, sizeof *out);
    if (!path || !*path || !game || !sha ||
        strlen(path) >= NVN_DISCOVERY_MAX_PATH_BYTES) return SHADER_PROFILE_ERROR;
    struct stat st;
    if (stat(path, &st)) return errno == ENOENT ? SHADER_PROFILE_MISSING : SHADER_PROFILE_ERROR;
    if (st.st_size < SHADER_PROFILE_HEADER_BYTES) return SHADER_PROFILE_CORRUPT;
    if ((uint64_t)st.st_size > SHADER_PROFILE_MAX_BYTES) return SHADER_PROFILE_LIMIT;
    size_t n = (size_t)st.st_size;
    uint8_t *bytes = malloc(n);
    if (!bytes) return SHADER_PROFILE_ERROR;
    FILE *f = fopen(path, "rb");
    if (!f) { free(bytes); return SHADER_PROFILE_ERROR; }
    bool ok = fread(bytes, 1, n, f) == n && !ferror(f) && fgetc(f) == EOF;
    if (fclose(f)) ok = false;
    ShaderProfileResult result = ok ? shader_profile_load_bytes(bytes, n, game, sha, out) : SHADER_PROFILE_ERROR;
    free(bytes);
    return result;
}

bool shader_profile_merge(const ShaderProfile *profile, NvnDiscoveryStore *store,
                          ShaderProfileResolve resolve, void *context,
                          const volatile int *cancel, ShaderProfileMergeStats *stats) {
    ShaderProfileMergeStats local = {0};
    if (!stats) stats = &local;
    memset(stats, 0, sizeof *stats);
    if (!profile_matches_store(profile, store) || !resolve) return false;
    ProfileKnown *known = profile_known_build(store);
    if (!known) return false;
    for (size_t i = 0; i < profile->count; i++) {
        if (cancel && *cancel) { stats->cancelled = true; break; }
        const ShaderProfileEntry *e = &profile->entries[i];
        if (!shader_profile_entry_valid(e)) { stats->rejected++; continue; }
        uint8_t metadata[SHADER_PROFILE_METADATA_BYTES], digest[32];
        if (!profile_encode(e, metadata) || !nvn_disc_sha(store->sha256, metadata, sizeof metadata, digest)) {
            stats->rejected++; continue;
        }
        if (profile_known_find(known, store, metadata, digest)) { stats->reused++; continue; }
        NvnDiscoveryRecipe r = {.rec = e->rec, .vkey = e->vkey, .fkey = e->fkey};
        memcpy(r.code_bytes, e->code_bytes, sizeof r.code_bytes);
        memcpy(r.constant_bytes, e->constant_bytes, sizeof r.constant_bytes);
        bool found = true;
        for (unsigned stage = 0; stage < 2; stage++) {
            if (cancel && *cancel) { found = false; break; }
            uint64_t hash = stage ? e->rec.fs : e->rec.vs;
            if (!hash) continue;
            uint8_t *code = NULL;
            unsigned kind = e->rec.kind == 'C' ? STAGE_COMPUTE : stage ? STAGE_FRAGMENT : STAGE_VERTEX;
            bool got = resolve(context, kind, hash, e->code_bytes[stage], e->constant_bytes[stage], &code);
            r.code[stage] = code;
            if (!got || !code) { found = false; break; }
        }
        if (cancel && *cancel) {
            stats->cancelled = true;
            nvn_discovery_recipe_release(&r);
            break;
        }
        if (!found) stats->missing++;
        else if (!nvn_disc_recipe_valid(&r)) stats->rejected++;
        else {
            size_t new_offset = store->size + 4;
            NvnDiscoveryAppendResult added = nvn_discovery_append(store, &r);
            if (added == NVN_DISCOVERY_APPEND_APPENDED) {
                stats->appended++;
                if (!profile_known_add(known, store, new_offset)) { nvn_discovery_recipe_release(&r); free(known); return false; }
            }
            else if (added == NVN_DISCOVERY_APPEND_DUPLICATE) stats->reused++;
            else stats->rejected++;
        }
        nvn_discovery_recipe_release(&r);
    }
    free(known);
    return true;
}

static bool profile_atomic_write(const char *path, const void *data, size_t size) {
    if (!path || !*path || strlen(path) + 12 >= NVN_DISCOVERY_MAX_PATH_BYTES) return false;
    /* Export may be called after a clean shutdown as well as after new learning.
     * Avoid replacing/fsyncing an identical metadata profile on every launch. */
    struct stat st;
    if (!stat(path, &st) && st.st_size >= 0 && (uint64_t)st.st_size == size) {
        FILE *existing = fopen(path, "rb");
        if (existing) {
            uint8_t chunk[4096];
            bool same = true;
            for (size_t at = 0; at < size;) {
                size_t n = size - at < sizeof chunk ? size - at : sizeof chunk;
                if (fread(chunk, 1, n, existing) != n || memcmp(chunk, (const uint8_t *)data + at, n)) {
                    same = false; break;
                }
                at += n;
            }
            if (fclose(existing)) same = false;
            if (same) return true;
        }
    }
    size_t n = strlen(path);
    char *tmp = malloc(n + 12);
    if (!tmp) return false;
    memcpy(tmp, path, n); memcpy(tmp + n, ".tmp.XXXXXX", 12);
    int fd = mkstemp(tmp);
    if (fd < 0) { free(tmp); return false; }
    bool ok = true;
    for (size_t at = 0; at < size;) {
        ssize_t written = write(fd, (const uint8_t *)data + at, size - at);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        at += (size_t)written;
    }
    if (ok && fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (ok && rename(tmp, path)) ok = false;
    if (!ok) unlink(tmp);
    free(tmp);
    return ok;
}

bool shader_profile_export(const char *path, NvnDiscoveryStore *store) {
    if (!store || !store->initialized || !store->sha256 || store->count > NVN_DISCOVERY_MAX_RECORDS) return false;
    size_t size = SHADER_PROFILE_HEADER_BYTES + (size_t)store->count * SHADER_PROFILE_RECORD_BYTES;
    uint8_t *bytes = calloc(1, size);
    if (!bytes) return false;
    memcpy(bytes, profile_magic, 8);
    nvn_disc_put32(bytes + 8, SHADER_PROFILE_SCHEMA);
    nvn_disc_put32(bytes + 12, SHADER_PROFILE_HEADER_BYTES);
    nvn_disc_put32(bytes + 16, store->count);
    nvn_disc_put32(bytes + 20, SHADER_PROFILE_RECORD_BYTES);
    memcpy(bytes + 24, store->game_id, 32);
    bool ok = true;
    for (uint32_t i = 0; i < store->count; i++) {
        NvnDiscoveryRecipe r = {0};
        if (!nvn_discovery_read_at(store, i, &r)) { ok = false; break; }
        ShaderProfileEntry e = {.rec = r.rec, .vkey = r.vkey, .fkey = r.fkey};
        memcpy(e.code_bytes, r.code_bytes, sizeof e.code_bytes);
        memcpy(e.constant_bytes, r.constant_bytes, sizeof e.constant_bytes);
        uint8_t *record = bytes + SHADER_PROFILE_HEADER_BYTES + i * SHADER_PROFILE_RECORD_BYTES;
        ok = profile_encode(&e, record) &&
             nvn_disc_sha(store->sha256, record, SHADER_PROFILE_METADATA_BYTES, record + SHADER_PROFILE_METADATA_BYTES);
        nvn_discovery_recipe_release(&r);
        if (!ok) break;
    }
    if (ok) ok = nvn_disc_sha(store->sha256, bytes + SHADER_PROFILE_HEADER_BYTES,
                              size - SHADER_PROFILE_HEADER_BYTES, bytes + 56) &&
                 nvn_disc_sha(store->sha256, bytes, 88, bytes + 88) && profile_atomic_write(path, bytes, size);
    free(bytes);
    return ok;
}

static bool profile_union_add(uint8_t *bytes, uint32_t *slots, uint32_t *count,
                              const ShaderProfileEntry *entry, NvnDiscoverySha256 sha) {
    uint8_t metadata[SHADER_PROFILE_METADATA_BYTES], digest[32];
    if (!profile_encode(entry, metadata) || !nvn_disc_sha(sha, metadata, sizeof metadata, digest)) return false;
    size_t slot = profile_known_slot(digest);
    for (size_t probes = 0; probes < PROFILE_KNOWN_SLOTS; probes++) {
        if (!slots[slot]) {
            if (*count == NVN_DISCOVERY_MAX_RECORDS) return false;
            uint8_t *record = bytes + SHADER_PROFILE_HEADER_BYTES + (size_t)*count * SHADER_PROFILE_RECORD_BYTES;
            memcpy(record, metadata, sizeof metadata); memcpy(record + sizeof metadata, digest, sizeof digest);
            slots[slot] = ++*count;
            return true;
        }
        const uint8_t *record = bytes + SHADER_PROFILE_HEADER_BYTES + (size_t)(slots[slot] - 1) * SHADER_PROFILE_RECORD_BYTES;
        if (!memcmp(record + SHADER_PROFILE_METADATA_BYTES, digest, 32) &&
            !memcmp(record, metadata, sizeof metadata)) return true;
        slot = (slot + 1) & (PROFILE_KNOWN_SLOTS - 1);
    }
    return false;
}

bool shader_profile_import_union(const char *path, NvnDiscoveryStore *store, const ShaderProfile *imported) {
    if (!store || !store->initialized || !store->sha256 || store->count > NVN_DISCOVERY_MAX_RECORDS) return false;
    if (imported && !profile_matches_store(imported, store)) return false;
    ShaderProfile existing = {0};
    ShaderProfileResult loaded = shader_profile_load(path, store->game_id, store->sha256, &existing);
    if (loaded != SHADER_PROFILE_MISSING && loaded != SHADER_PROFILE_LOADED) return false;
    uint8_t *bytes = calloc(1, SHADER_PROFILE_MAX_BYTES);
    uint32_t *slots = calloc(PROFILE_KNOWN_SLOTS, sizeof *slots);
    bool ok = bytes && slots;
    uint32_t count = 0;
    for (size_t i = 0; ok && i < existing.count; i++)
        ok = profile_union_add(bytes, slots, &count, &existing.entries[i], store->sha256);
    for (size_t i = 0; imported && ok && i < imported->count; i++)
        ok = profile_union_add(bytes, slots, &count, &imported->entries[i], store->sha256);
    for (size_t i = 0; ok && i < store->count; i++) {
        NvnDiscoveryRecipe r = {0};
        if (!nvn_discovery_read_at(store, i, &r)) { ok = false; break; }
        ShaderProfileEntry e = {.rec = r.rec, .vkey = r.vkey, .fkey = r.fkey};
        memcpy(e.code_bytes, r.code_bytes, sizeof e.code_bytes);
        memcpy(e.constant_bytes, r.constant_bytes, sizeof e.constant_bytes);
        ok = profile_union_add(bytes, slots, &count, &e, store->sha256);
        nvn_discovery_recipe_release(&r);
    }
    if (ok) {
        size_t size = SHADER_PROFILE_HEADER_BYTES + (size_t)count * SHADER_PROFILE_RECORD_BYTES;
        memcpy(bytes, profile_magic, 8);
        nvn_disc_put32(bytes + 8, SHADER_PROFILE_SCHEMA);
        nvn_disc_put32(bytes + 12, SHADER_PROFILE_HEADER_BYTES);
        nvn_disc_put32(bytes + 16, count);
        nvn_disc_put32(bytes + 20, SHADER_PROFILE_RECORD_BYTES);
        memcpy(bytes + 24, store->game_id, 32);
        ok = nvn_disc_sha(store->sha256, bytes + SHADER_PROFILE_HEADER_BYTES,
                           size - SHADER_PROFILE_HEADER_BYTES, bytes + 56) &&
             nvn_disc_sha(store->sha256, bytes, 88, bytes + 88) && profile_atomic_write(path, bytes, size);
    }
    free(slots); free(bytes); shader_profile_close(&existing);
    return ok;
}

bool shader_profile_export_union(const char *path, NvnDiscoveryStore *store) {
    return shader_profile_import_union(path, store, NULL);
}
