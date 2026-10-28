/* Metadata-only profile maintenance. Never prints original code/constants. */
#include "shader_profile.h"
bool shared_sha256_bytes(const void *, size_t, uint8_t[32]);

static bool identity_read(const char *path, size_t header_bytes, const char magic[8], uint8_t identity[32]) {
    uint8_t header[SHADER_PROFILE_HEADER_BYTES];
    if (header_bytes > sizeof header) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(header, 1, header_bytes, f) == header_bytes && !ferror(f);
    if (fclose(f)) ok = false;
    if (!ok || memcmp(header, magic, 8)) return false;
    memcpy(identity, header + 24, 32);
    return true;
}

static void describe(const char *action, const ShaderProfile *profile, bool recovered) {
    size_t graphics = 0, compute = 0, depth = 0, typed = 0;
    for (size_t i = 0; i < profile->count; i++) {
        const ShaderProfileEntry *entry = &profile->entries[i];
        if (entry->rec.kind == 'C') compute++; else graphics++;
        if (entry->rec.kind == 'G' && !entry->rec.fs) depth++;
        if (entry->vkey.tex_uint || entry->fkey.tex_uint) typed++;
    }
    printf("{\"action\":\"%s\",\"schema\":%u,\"game_version_identity\":\"", action, SHADER_PROFILE_SCHEMA);
    for (unsigned i = 0; i < 32; i++) printf("%02x", profile->game_id[i]);
    printf("\",\"records\":%zu,\"graphics\":%zu,\"compute\":%zu,\"depth_only\":%zu,"
           "\"typed_texture_variants\":%zu,\"shader_payload_included\":false,\"journal_recovered_prefix\":%s}\n",
           profile->count, graphics, compute, depth, typed, recovered ? "true" : "false");
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: shader-profile-tool inspect PROFILE | export JOURNAL OUTPUT\n"); return 2; }
    uint8_t identity[32]; ShaderProfile profile = {0};
    if (!strcmp(argv[1], "inspect") && argc == 3) {
        if (!identity_read(argv[2], SHADER_PROFILE_HEADER_BYTES, "NVNPROF", identity) ||
            shader_profile_load(argv[2], identity, shared_sha256_bytes, &profile) != SHADER_PROFILE_LOADED) {
            fprintf(stderr, "invalid or unsupported shader profile\n"); return 1;
        }
        describe("inspected", &profile, false); shader_profile_close(&profile); return 0;
    }
    if (!strcmp(argv[1], "export") && argc == 4) {
        if (!identity_read(argv[2], NVN_DISCOVERY_HEADER_BYTES, "NVNDISC", identity)) {
            fprintf(stderr, "invalid shader discovery journal header\n"); return 1;
        }
        NvnDiscoveryStore store = {0};
        NvnDiscoveryOpenResult loaded = nvn_discovery_open(&store, argv[2], identity, shared_sha256_bytes,
            32u * 1024u * 1024u, NVN_DISCOVERY_MAX_RECORDS);
        if (loaded != NVN_DISCOVERY_OPEN_LOADED && loaded != NVN_DISCOVERY_OPEN_RECOVERED) {
            fprintf(stderr, "journal validation failed (%d); no profile written\n", loaded); return 1;
        }
        bool ok = shader_profile_export_union(argv[3], &store);
        nvn_discovery_close(&store);
        if (!ok || shader_profile_load(argv[3], identity, shared_sha256_bytes, &profile) != SHADER_PROFILE_LOADED) {
            fprintf(stderr, "profile export failed; incompatible/corrupt existing destinations are preserved\n"); return 1;
        }
        describe("exported", &profile, loaded == NVN_DISCOVERY_OPEN_RECOVERED);
        shader_profile_close(&profile); return 0;
    }
    fprintf(stderr, "invalid shader profile command\n"); return 2;
}
