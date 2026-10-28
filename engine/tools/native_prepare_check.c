/* Host-only preparation helper: actual title policy and original NSO decoder. */
#include "adapters/title_adapter.h"
#include "runtime/aot_cpu.h"
#include "runtime/nso_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const TitleAdapter *adapter(const char *key) {
    const enum TitleId ids[] = {TITLE_LAS, TITLE_EEVEE, TITLE_ECHOES, TITLE_WONDER};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; ++i) {
        const TitleAdapter *a = title_adapter_find(ids[i]);
        if (a && !strcmp(a->key, key)) return a;
    }
    return NULL;
}
static int problem(const char *why) { fprintf(stderr, "native preparation: %s\n", why); return 1; }
static void *read_file(const char *path, size_t limit, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || (unsigned long)size > limit || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *bytes = malloc(size ? (size_t)size : 1);
    if (!bytes) { fclose(f); return NULL; }
    if (fread(bytes, 1, (size_t)size, f) != (size_t)size) { free(bytes); fclose(f); return NULL; }
    fclose(f); *length = (size_t)size; return bytes;
}
static void policy(const TitleAdapter *a) {
    printf("{\"title_id\":%u,\"program_id\":%llu,\"guest_shift\":%llu,\"window_lo\":%llu,\"window_hi\":%llu,"
        "\"target\":%u,\"cpu_identity\":%u,\"cpu_size\":%zu,\"cpu_alignment\":%zu,\"mapping_alignment\":16384,\"modules\":[",
        (unsigned)a->id, (unsigned long long)a->program_id, (unsigned long long)a->guest_shift,
        (unsigned long long)(a->canonical_window_lo-a->guest_shift), (unsigned long long)(a->canonical_window_hi-a->guest_shift),
        MODULE_ABI_TARGET_ANDROID_ARM64_LP64, MODULE_ABI_CPU_A64_SHARED_66560, sizeof(Cpu), _Alignof(Cpu));
    for (unsigned i = 0; i < a->module_count; ++i) {
        printf("%s{\"name\":\"%s\",\"module_id\":%u,\"canonical_base\":%llu,\"build_prefix\":\"", i ? "," : "",
            a->modules[i].name, a->modules[i].module_id, (unsigned long long)a->modules[i].canonical_base);
        unsigned size = a->module_build_prefix_size[i];
        const uint8_t *prefix = a->module_build_prefix[i];
        if (!size && a->modules[i].module_id == MODULE_ABI_MAIN) { size = a->main_build_prefix_size; prefix = a->main_build_prefix; }
        for (unsigned j = 0; j < size; ++j) printf("%02x",prefix[j]);
        fputs("\"}",stdout);
    }
    puts("]}");
}
int main(int argc, char **argv) {
    if (argc < 3) return problem("usage: policy TITLE | validate TITLE EXEFS MANIFEST CONTRACTHEX");
    const TitleAdapter *a = adapter(argv[2]);
    if (!a) return problem("unknown title");
    if (argc == 3 && !strcmp(argv[1], "policy")) { policy(a); return 0; }
    if (argc != 6 || strcmp(argv[1], "validate") || strlen(argv[5]) != 64) return problem("invalid arguments");
    uint8_t contract[32];
    for (unsigned i = 0; i < 32; ++i) {
        unsigned value;
        char pair[] = {argv[5][2*i], argv[5][2*i+1], 0};
        if (strspn(pair,"0123456789abcdefABCDEF") != 2 || sscanf(pair,"%2x",&value) != 1) return problem("invalid contract hex");
        contract[i] = (uint8_t)value;
    }
    size_t length;
    uint8_t *wire = read_file(argv[4], NATIVE_MANIFEST_MAX_SIZE, &length);
    if (!wire) return problem("cannot read bounded manifest");
    NativeManifest manifest; NativeManifestFailure nf;
    enum NativeManifestStatus ns = native_manifest_parse(wire, length, contract, &manifest, &nf);
    free(wire);
    if (ns != NATIVE_MANIFEST_OK) return problem(native_manifest_status_string(ns));
    SessionLayoutSpec layout; TitleAdapterFailure af;
    enum TitleAdapterStatus as = title_adapter_build_native_layout(a->id,&manifest,contract,16384,&layout,&af);
    if (as != TITLE_ADAPTER_OK) return problem(title_adapter_status_string(as));
    for (unsigned i = 0; i < manifest.module_count; ++i) {
        if (manifest.modules[i].mapped_size > (256u << 20)) return problem("image exceeds bounded preparation limit");
        char path[4096];
        int n = snprintf(path,sizeof path,"%s/%s",argv[3],a->modules[i].name);
        if (n < 0 || (size_t)n >= sizeof path) return problem("module path too long");
        uint8_t *raw = read_file(path,512u << 20,&length);
        if (!raw) return problem("cannot read bounded original NSO");
        NsoImage image; NsoImageFailure f;
        enum NsoImageStatus status = nso_image_decode_native(raw,length,&manifest.modules[i],&manifest.identity,
            manifest.mapping_alignment,&image,&f);
        free(raw);
        if (status != NSO_IMAGE_OK) return problem(nso_image_status_string(status));
        nso_image_destroy(&image);
    }
    printf("{\"state\":\"validated\",\"modules\":%u,\"generated_functions\":false}\n",manifest.module_count);
    return 0;
}
