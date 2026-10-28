/* Synthetic generic-adapter checks: derivation from in-memory ExeFS metadata,
 * rejection reasons, layout preflight, native preparation, package
 * classification/metadata JSON and once-per-process install/selection.
 * No game files: NSO/NPDM bytes are built from the documented header layout. */
#define _GNU_SOURCE
#include "../ports/shared-android/runtime/nxpkg.c"
#include "../ports/shared-android/runtime/native_package_metadata.h"
#include "../native/adapters/generic/generic.h"
#include "../native/runtime/native_prepare.h"
#include "../native/runtime/native_manifest.h"
#include "../native/runtime/aot_cpu.h"
#include <inttypes.h>
#include <sys/stat.h>

extern bool shared_sha256_bytes(const void *, size_t, uint8_t[32]);
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "generic test line %d: %s\n", __LINE__, #x); abort(); } } while (0)

#define PROGRAM UINT64_C(0x0100aaaabbbb0000)
typedef struct File { char name[32]; uint8_t data[0x400]; size_t size; } File;
typedef struct Exefs { File files[8]; unsigned count; } Exefs;

static void p32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void p64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static File *add(Exefs *e, const char *name, size_t size) {
    File *f = &e->files[e->count++];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->size = size;
    return f;
}
/* Uncompressed NSO0 whose segments decode (as tests/native_prepare_test.c). */
static void nso(Exefs *e, const char *name, uint8_t tag, uint32_t bss) {
    File *f = add(e, name, 0x200);
    uint8_t *raw = f->data;
    memcpy(raw, "NSO0", 4);
    for (unsigned j = 0; j < 3; j++) {
        p32(raw + 0x10 + j * 16, 0x100 + 0x20 * j);
        p32(raw + 0x14 + j * 16, 0x1000 * j);
        p32(raw + 0x18 + j * 16, 16);
        p32(raw + 0x60 + 4 * j, 16);
        for (unsigned k = 0; k < 16; k++) raw[0x100 + 0x20 * j + k] = (uint8_t)(tag + j + k);
    }
    p32(raw + 0x3c, bss);
    for (unsigned i = 0; i < 32; i++) raw[0x40 + i] = (uint8_t)(tag * 7 + i + 1);
}
static File *npdm(Exefs *e, uint64_t program, uint8_t flags) {
    File *f = add(e, "main.npdm", 0x100);
    uint8_t *m = f->data;
    memcpy(m, "META", 4);
    m[0xc] = flags; m[0xe] = 44; m[0xf] = 0;
    p32(m + 0x14, 0x400000);
    p32(m + 0x1c, 0x100000);
    memcpy(m + 0x20, "Synthetic", 9);
    p32(m + 0x70, 0x80); p32(m + 0x74, 0x40);
    memcpy(m + 0x80, "ACI0", 4);
    p64(m + 0x90, program);
    return f;
}
static void standard(Exefs *e, bool subsdk) {
    memset(e, 0, sizeof *e);
    add(e, "rtld", 0x200);
    nso(e, "main", 1, 0x2000);
    if (subsdk) { nso(e, "subsdk0", 2, 0); nso(e, "subsdk1", 3, 0); }
    nso(e, "sdk", 4, 0x4000000);
    npdm(e, PROGRAM, 0x07);
}
static File *find(Exefs *e, const char *name) {
    for (unsigned i = 0; i < e->count; i++) if (!strcmp(e->files[i].name, name)) return &e->files[i];
    return NULL;
}
static bool mem_list(void *context, TitleGenericVisit visit, void *visit_context) {
    Exefs *e = context;
    for (unsigned i = 0; i < e->count; i++) if (!visit(visit_context, e->files[i].name, e->files[i].size)) return false;
    return true;
}
static bool mem_read(void *context, const char *name, uint64_t offset, void *dst, size_t size) {
    File *f = find(context, name);
    if (!f || offset > f->size || size > f->size - offset) return false;
    memcpy(dst, f->data + offset, size);
    return true;
}
static bool derive(Exefs *e, uint64_t expected, TitleGenericAdapter *g, char *why) {
    TitleGenericSource source = {e, mem_list, mem_read};
    TitleGenericOptions options = {.expected_program_id = expected, .display_version = "1.2.0"};
    return title_generic_derive(&source, &options, g, why, 256);
}
static void reject(Exefs *e, const char *needle) {
    TitleGenericAdapter g;
    char why[256];
    memset(&g, 0xa5, sizeof g);
    CHECK(!derive(e, PROGRAM, &g, why));
    if (!strstr(why, needle)) fprintf(stderr, "reason was: %s\n", why);
    CHECK(strstr(why, needle));
    TitleGenericAdapter zero;
    memset(&zero, 0, sizeof zero);
    CHECK(!memcmp(&g, &zero, sizeof g));
}

static NativeManifest manifest_for(const TitleAdapter *a, Exefs *e) {
    NativeManifest m = {.struct_size = sizeof m, .version = NATIVE_MANIFEST_VERSION, .title_id = a->id,
        .module_count = a->module_count, .program_id = a->program_id, .mapping_alignment = 16384,
        .identity = {.struct_size = sizeof(ModuleAbiIdentity), .abi_version = MODULE_ABI_VERSION,
            .target = MODULE_ABI_TARGET_ANDROID_ARM64_LP64, .cpu_identity = MODULE_ABI_CPU_A64_SHARED_66560,
            .cpu_size = sizeof(Cpu), .cpu_alignment = _Alignof(Cpu), .guest_shift = a->guest_shift,
            .guest_window_lo = a->canonical_window_lo - a->guest_shift, .guest_window_hi = a->canonical_window_hi - a->guest_shift}};
    memset(m.contract_sha256, 0x5a, 32);
    for (uint32_t i = 0; i < a->module_count; i++) {
        File *f = find(e, a->modules[i].name);
        NativeModuleSpec *s = &m.modules[i];
        s->struct_size = sizeof *s; s->module_id = a->modules[i].module_id; s->canonical_base = a->modules[i].canonical_base;
        memcpy(s->nso_build_id, f->data + 0x40, 32);
        memset(s->nso_sha256, (int)(i + 1), 32);
        s->text_offset = 0; s->text_size = 16;
        s->mapped_size = a->module_spans[i] < 0x8000 ? a->module_spans[i] : 0x8000;
    }
    return m;
}

static void derivation_tests(void) {
    Exefs e;
    TitleGenericAdapter g;
    char why[256];
    /* Two-module package (main, sdk, rtld ignored). */
    standard(&e, false);
    CHECK(derive(&e, PROGRAM, &g, why));
    const TitleAdapter *a = &g.adapter, *echoes = title_adapter_find(TITLE_ECHOES);
    CHECK(a->id == TITLE_GENERIC && !strcmp(a->key, "generic") && a->program_id == PROGRAM);
    CHECK(a->version == g.version && !strcmp(a->version, "1.2.0"));
    CHECK(a->save_namespace == g.save_namespace && !strcmp(a->save_namespace, "generic-0100aaaabbbb0000-1.2.0"));
    CHECK(a->module_count == 2 && a->capabilities == 0 && a->renderer_policy_flags == 0 && a->audout_min_queue_ms == 0);
    CHECK(a->nvn_api_major == 0 && a->nvn_api_minor == 0);
    CHECK(a->guest_shift == echoes->guest_shift && a->canonical_window_lo + UINT64_C(0x1000000) == echoes->canonical_window_lo &&
          a->canonical_window_hi == echoes->canonical_window_hi && !memcmp(a->regions, echoes->regions, sizeof a->regions));
    CHECK(a->modules[0].module_id == MODULE_ABI_MAIN && !strcmp(a->modules[0].name, "main") && a->modules[0].canonical_base == UINT64_C(0x7000000000));
    CHECK(a->modules[1].module_id == MODULE_ABI_SDK && !strcmp(a->modules[1].name, "sdk") && a->modules[1].canonical_base == UINT64_C(0x7002000000));
    /* sdk BSS 0x4000000 + 0x2010 -> 0x4004000 mapped, 0x6000000 span */
    CHECK(a->module_spans[0] == UINT64_C(0x2000000) && a->module_spans[1] == UINT64_C(0x6000000));
    CHECK(a->main_build_prefix_size == 20 && !memcmp(a->main_build_prefix, find(&e, "main")->data + 0x40, 20));
    CHECK(a->module_build_prefix_size[1] == 20 && !memcmp(a->module_build_prefix[1], find(&e, "sdk")->data + 0x40, 20));
    CHECK(a->startup.argument_count == 0 && a->startup.early_count == 1 && a->startup.early_modules[0] == MODULE_ABI_SDK);
    CHECK(a->startup.late_count == 1 && a->startup.late_modules[0] == MODULE_ABI_MAIN);
    CHECK(g.main_thread_priority == 44 && g.main_thread_core == 0 && g.main_thread_stack_size == 0x100000);
    CHECK(g.system_resource_size == 0x400000 && g.address_space == 3 && !strcmp(g.npdm_name, "Synthetic") && g.rtld_present);
    char line[1024];
    title_generic_describe(&g, line, sizeof line);
    CHECK(strstr(line, "[generic] no Raitchi adapter for program 0100aaaabbbb0000 version 1.2.0"));
    CHECK(strstr(line, "modules=main@7000000000+2000000,sdk@7002000000+6000000"));
    CHECK(strstr(line, "NVN API 53.207 (assumed default)") && strstr(line, "rtld=present,not-loaded"));
    CHECK(strstr(line, "nn::init::Start argument count from the SDK export") && strstr(line, "save=generic-0100aaaabbbb0000-1.2.0"));
    /* Layout preflight through the shared helper, before and without install. */
    NativeManifest m = manifest_for(a, &e);
    uint8_t contract[32];
    memset(contract, 0x5a, 32);
    SessionLayoutSpec spec;
    TitleAdapterFailure failure;
    CHECK(title_adapter_build_native_layout_for(a, &m, contract, 4096, &spec, &failure) == TITLE_ADAPTER_OK);
    CHECK(spec.module_count == 2 && spec.guest_shift == echoes->guest_shift && spec.modules[1].image.canonical_base == UINT64_C(0x7002000000));
    SessionLayout normalized;
    CHECK(session_layout_build(&spec, &normalized, NULL) == SESSION_LAYOUT_OK);
    CHECK(normalized.window.lo + UINT64_C(0x1000000) == normalized.modules[0].image.lo);
    CHECK(normalized.modules[0].image.lo == UINT64_C(0x7000000000) - echoes->guest_shift);
    for (unsigned r = 0; r < SESSION_REGION_COUNT; r++) {
        CHECK(spec.regions[r].canonical_base == echoes->regions[r].canonical_base && spec.regions[r].size == echoes->regions[r].size);
        CHECK(normalized.regions[r].lo == echoes->regions[r].canonical_base - echoes->guest_shift);
    }
    NativeManifest bad = m;
    bad.modules[1].nso_build_id[3] ^= 1;
    CHECK(title_adapter_build_native_layout_for(a, &bad, contract, 4096, &spec, &failure) == TITLE_ADAPTER_BUILD);
    bad = m; bad.program_id += 0x1000;
    CHECK(title_adapter_build_native_layout_for(a, &bad, contract, 4096, &spec, &failure) == TITLE_ADAPTER_PROFILE);
    bad = m; bad.modules[0].mapped_size = UINT64_C(0x2004000);
    CHECK(title_adapter_build_native_layout_for(a, &bad, contract, 4096, &spec, &failure) == TITLE_ADAPTER_PROFILE);
    bad = m; bad.title_id = TITLE_ECHOES;
    CHECK(title_adapter_build_native_layout_for(a, &bad, contract, 4096, &spec, &failure) == TITLE_ADAPTER_PROFILE);
    CHECK(title_adapter_build_native_layout_for(NULL, &m, contract, 4096, &spec, &failure) == TITLE_ADAPTER_ARGUMENT);

    /* Four-module package: standard load order, reverse late init. */
    standard(&e, true);
    CHECK(derive(&e, 0, &g, why));
    a = &g.adapter;
    CHECK(a->module_count == 4);
    const uint32_t ids[] = {MODULE_ABI_MAIN, MODULE_ABI_SUBSDK0, MODULE_ABI_SUBSDK1, MODULE_ABI_SDK};
    for (unsigned i = 0; i < 4; i++) CHECK(a->modules[i].module_id == ids[i] && a->modules[i].canonical_base == UINT64_C(0x7000000000) + i * UINT64_C(0x2000000));
    CHECK(a->startup.late_count == 3 && a->startup.late_modules[0] == MODULE_ABI_SUBSDK1 &&
          a->startup.late_modules[1] == MODULE_ABI_SUBSDK0 && a->startup.late_modules[2] == MODULE_ABI_MAIN);
    m = manifest_for(a, &e);
    CHECK(title_adapter_build_native_layout_for(a, &m, contract, 16384, &spec, &failure) == TITLE_ADAPTER_OK);
    CHECK(session_layout_build(&spec, &normalized, NULL) == SESSION_LAYOUT_OK);
    CHECK(normalized.window.lo + UINT64_C(0x1000000) == normalized.modules[0].image.lo);
    for (unsigned i = 0; i < 4; i++)
        CHECK(normalized.modules[i].image.lo == UINT64_C(0x7000000000) + i * UINT64_C(0x2000000) - echoes->guest_shift);

    /* Options: NVN override and version token. */
    TitleGenericSource source = {&e, mem_list, mem_read};
    TitleGenericOptions options = {.display_version = "v1.0 (Ä)/..", .nvn_api_major = 55, .nvn_api_minor = 15};
    CHECK(title_generic_derive(&source, &options, &g, why, sizeof why));
    CHECK(g.adapter.nvn_api_major == 55 && g.adapter.nvn_api_minor == 15 && !strcmp(g.version, "v1.0______.."));
    title_generic_describe(&g, line, sizeof line);
    CHECK(strstr(line, "NVN API 55.15 (override)"));
    char token[17];
    title_generic_version_token("", token); CHECK(!strcmp(token, "0"));
    title_generic_version_token(NULL, token); CHECK(!strcmp(token, "0"));
    title_generic_version_token("12345678901234567890", token); CHECK(!strcmp(token, "1234567890123456"));
    uint32_t major, minor;
    CHECK(title_generic_parse_nvn_api("55.15", &major, &minor) && major == 55 && minor == 15);
    CHECK(title_generic_parse_nvn_api("53.207", &major, &minor) && major == 53 && minor == 207);
    CHECK(!title_generic_parse_nvn_api("55", &major, &minor) && !major && !minor);
    CHECK(!title_generic_parse_nvn_api("0.1", &major, &minor) && !title_generic_parse_nvn_api("55.15x", &major, &minor));
    CHECK(!title_generic_parse_nvn_api("1234.1", &major, &minor) && !title_generic_parse_nvn_api(NULL, &major, &minor));
}

static void rejection_tests(void) {
    Exefs e;
    standard(&e, false); e.files[1].name[0] = 'x'; reject(&e, "Unexpected or duplicate ExeFS file: xain");
    standard(&e, false); add(&e, "subsdk2", 0x200); reject(&e, "ExeFS module subsdk2 is beyond");
    standard(&e, false); add(&e, "main", 0x200); reject(&e, "Unexpected or duplicate ExeFS file: main");
    standard(&e, false); snprintf(find(&e, "sdk")->name, 32, "sdx"); reject(&e, "Unexpected or duplicate ExeFS file: sdx");
    memset(&e, 0, sizeof e); nso(&e, "main", 1, 0); npdm(&e, PROGRAM, 7); reject(&e, "no sdk module");
    memset(&e, 0, sizeof e); nso(&e, "sdk", 1, 0); npdm(&e, PROGRAM, 7); reject(&e, "no main module");
    memset(&e, 0, sizeof e); nso(&e, "main", 1, 0); nso(&e, "sdk", 2, 0); reject(&e, "no main.npdm");
    standard(&e, false); find(&e, "main.npdm")->data[0xc] = 0x06; reject(&e, "32-bit program");
    standard(&e, false); find(&e, "main.npdm")->data[0xc] = 0x05; reject(&e, "32-bit program");
    standard(&e, false); find(&e, "main.npdm")->data[0xc] = 0x03; reject(&e, "36-bit address space");
    standard(&e, false); find(&e, "main.npdm")->data[0] = 'X'; reject(&e, "META header");
    standard(&e, false); find(&e, "main.npdm")->data[0x80] = 'X'; reject(&e, "ACI0 header");
    standard(&e, false); p32(find(&e, "main.npdm")->data + 0x70, 0xf0); reject(&e, "ACI0 header");
    standard(&e, false); find(&e, "main.npdm")->data[0xe] = 64; reject(&e, "priority, core or stack");
    standard(&e, false); find(&e, "main.npdm")->data[0xf] = 4; reject(&e, "priority, core or stack");
    standard(&e, false); p32(find(&e, "main.npdm")->data + 0x1c, 0x100800); reject(&e, "priority, core or stack");
    standard(&e, false); p32(find(&e, "main.npdm")->data + 0x1c, 0); reject(&e, "priority, core or stack");
    standard(&e, false); p64(find(&e, "main.npdm")->data + 0x90, PROGRAM | 0x800); reject(&e, "Only base application programs");
    standard(&e, false); p64(find(&e, "main.npdm")->data + 0x90, PROGRAM | 0x1); reject(&e, "Only base application programs");
    standard(&e, false); p64(find(&e, "main.npdm")->data + 0x90, 0); reject(&e, "no program ID");
    standard(&e, false); p64(find(&e, "main.npdm")->data + 0x90, PROGRAM + 0x2000); reject(&e, "differs from the package");
    for (int id = TITLE_LAS; id <= TITLE_WONDER; id++) {
        standard(&e, false);
        const TitleAdapter *known = title_adapter_find((enum TitleId)id);
        p64(find(&e, "main.npdm")->data + 0x90, known->program_id);
        TitleGenericAdapter g; char why[256];
        CHECK(!derive(&e, known->program_id, &g, why) && strstr(why, "A specific Raitchi adapter exists") && strstr(why, known->key));
    }
    standard(&e, false); find(&e, "sdk")->data[0] = 'X'; reject(&e, "Invalid NSO header in ExeFS module sdk");
    standard(&e, false); find(&e, "main")->size = 0x80; reject(&e, "Invalid NSO header in ExeFS module main");
    standard(&e, false); memset(find(&e, "sdk")->data + 0x40, 0, 32); reject(&e, "sdk has no build ID");
    standard(&e, false); p32(find(&e, "main")->data + 0x18, 0); reject(&e, "exceed the native preparation policy");
    standard(&e, false); p32(find(&e, "main")->data + 0x3c, 0x10000000); reject(&e, "exceed the native preparation policy");
    standard(&e, false); p32(find(&e, "main")->data + 0x24, 0x3000); reject(&e, "Unsupported NSO segment order");
    /* Four modules at the 256 MiB preparation limit still fit below the HLE region. */
    standard(&e, true);
    for (unsigned i = 0; i < e.count; i++) if (!memcmp(e.files[i].data, "NSO0", 4)) p32(e.files[i].data + 0x3c, 0xff00000);
    TitleGenericAdapter g; char why[256];
    CHECK(derive(&e, PROGRAM, &g, why));
    TitleGenericSource broken = {&e, NULL, mem_read};
    TitleGenericOptions options = {0};
    CHECK(!title_generic_derive(&broken, &options, &g, why, sizeof why) && strstr(why, "Invalid generic adapter arguments"));
    CHECK(!title_generic_derive_directory("/nonexistent-generic-exefs", &options, &g, why, sizeof why) && strstr(why, "ExeFS file table"));
}

/* Synthetic NxPackage with an unencrypted PFS0 ExeFS (enc_type 1), as in package_metadata_probe.c. */
static int package_fd(const Exefs *e, uint64_t *size) {
    char name[] = "/tmp/raitchi-generic-XXXXXX";
    int fd = mkstemp(name);
    CHECK(fd >= 0);
    unlink(name);
    uint8_t table[0x400] = {0};
    size_t strings = 0;
    char names[256] = {0};
    for (unsigned i = 0; i < e->count; i++) { size_t n = strlen(e->files[i].name) + 1; memcpy(names + strings, e->files[i].name, n); strings += n; }
    strings = (strings + 15) & ~(size_t)15;
    size_t meta = 16 + e->count * 24 + strings, offset = 0;
    memcpy(table, "PFS0", 4); p32(table + 4, e->count); p32(table + 8, (uint32_t)strings);
    size_t name_offset = 0;
    for (unsigned i = 0; i < e->count; i++) {
        uint8_t *entry = table + 16 + i * 24;
        p64(entry, offset); p64(entry + 8, e->files[i].size); p32(entry + 16, (uint32_t)name_offset);
        name_offset += strlen(e->files[i].name) + 1;
        offset += e->files[i].size;
    }
    memcpy(table + 16 + e->count * 24, names, strings);
    CHECK(pwrite(fd, table, meta, 0) == (ssize_t)meta);
    offset = 0;
    for (unsigned i = 0; i < e->count; i++) {
        CHECK(pwrite(fd, e->files[i].data, e->files[i].size, (off_t)(meta + offset)) == (ssize_t)e->files[i].size);
        offset += e->files[i].size;
    }
    *size = meta + offset;
    return fd;
}
static NxPackage synthetic_package(int fd, uint64_t size, uint64_t title) {
    NxPackage p = {0};
    p.fd = fd; p.title_id = title;
    p.exefs = (Section){.off = 0, .size = size, .data_off = 0, .data_size = size, .enc_type = 1};
    return p;
}
static void classify(Exefs *e, uint64_t title, NativePackageClass *c, char **json) {
    uint64_t size;
    int fd = package_fd(e, &size);
    NxPackage p = synthetic_package(fd, size, title);
    NxPackageMetadata m = {0};
    snprintf(m.title, sizeof m.title, "Synthetic \"game\"");
    snprintf(m.developer, sizeof m.developer, "Test publisher");
    snprintf(m.version, sizeof m.version, "1.2.0");
    native_package_classify(&p, &m, c);
    *json = native_package_metadata_json(title, &m, "/icons/x.dat", c);
    CHECK(*json);
    close(fd);
}
static void package_tests(void) {
    Exefs e;
    NativePackageClass c;
    char *json;
    standard(&e, true);
    uint64_t size;
    int fd = package_fd(&e, &size);
    NxPackage p = synthetic_package(fd, size, PROGRAM);
    uint8_t header[0x100];
    CHECK(nx_package_exefs_read(&p, "sdk", 0, header, sizeof header) && !memcmp(header, find(&e, "sdk")->data, sizeof header));
    CHECK(nx_package_exefs_read(&p, "main.npdm", 0x80, header, 4) && !memcmp(header, "ACI0", 4));
    CHECK(!nx_package_exefs_read(&p, "sdk", 0x101, header, 0x100) && !nx_package_exefs_read(&p, "missing", 0, header, 1));
    close(fd);

    classify(&e, PROGRAM, &c, &json);
    CHECK(c.tryable && !c.compatible && !c.adapter && !strcmp(c.adapter_id, "generic"));
    CHECK(!strcmp(c.reason, NATIVE_PACKAGE_TRYABLE_REASON));
    CHECK(strstr(json, "\"adapterId\":\"generic\",\"compatible\":false,\"tryable\":true,\"reason\":\"No Raitchi adapter for this game yet; it can be tried with generic startup and may close unexpectedly\"}"));
    CHECK(strstr(json, "\"title\":\"Synthetic \\\"game\\\"\"") && strstr(json, "\"programIdHex\":\"0100aaaabbbb0000\""));
    free(json);

    nso(&e, "subsdk2", 5, 0);
    classify(&e, PROGRAM, &c, &json);
    CHECK(!c.tryable && !c.compatible && !strcmp(c.adapter_id, ""));
    CHECK(strstr(c.reason, "No Raitchi adapter matches this program ID; generic startup cannot try it: ExeFS module subsdk2 is beyond"));
    CHECK(strstr(json, "\"adapterId\":\"\",\"compatible\":false,\"tryable\":false"));
    free(json);

    standard(&e, false);
    classify(&e, PROGRAM + 0x1000, &c, &json); /* NCA program differs from NPDM */
    CHECK(!c.tryable && strstr(c.reason, "differs from the package program ID"));
    free(json);

    /* A known program ID keeps the specific adapter verdict and is never tryable. */
    const TitleAdapter *wonder = title_adapter_find(TITLE_WONDER);
    standard(&e, false);
    p64(find(&e, "main.npdm")->data + 0x90, wonder->program_id);
    classify(&e, wonder->program_id, &c, &json);
    CHECK(c.adapter == wonder && !c.tryable && !c.compatible && !strcmp(c.adapter_id, "wonder"));
    CHECK(!strcmp(c.reason, "Unsupported build identity: main"));
    CHECK(strstr(json, "\"adapterId\":\"wonder\",\"compatible\":false,\"tryable\":false,\"reason\":\"Unsupported build identity: main\"}"));
    free(json);
}

static void write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(data, 1, size, f) == size && !fclose(f));
}
static void prepare_tests(const char *root, const uint8_t contract[32]) {
    Exefs e;
    standard(&e, true);
    char dir[4096], exefs[4096], manifest[4096], path[4200], why[256];
    snprintf(dir, sizeof dir, "%s/generic", root);
    snprintf(exefs, sizeof exefs, "%s/exefs", dir);
    snprintf(manifest, sizeof manifest, "%s/native.manifest", dir);
    CHECK(!mkdir(dir, 0700) && !mkdir(exefs, 0700));
    for (unsigned i = 0; i < e.count; i++) {
        snprintf(path, sizeof path, "%s/%s", exefs, e.files[i].name);
        write_file(path, e.files[i].data, e.files[i].size);
    }
    CHECK(native_prepare_program_id("generic") == 0);
    NativePrepareResult result;
    CHECK(native_prepare_exefs("generic", exefs, manifest, contract, shared_sha256_bytes, &result, why, sizeof why));
    CHECK(result.created && result.modules == 4 && result.program_id == PROGRAM && !strcmp(result.title, "generic"));
    uint8_t wire[NATIVE_MANIFEST_MAX_SIZE];
    FILE *f = fopen(manifest, "rb");
    CHECK(f);
    size_t size = fread(wire, 1, sizeof wire, f);
    fclose(f);
    NativeManifest m;
    NativeManifestFailure mf;
    CHECK(size == 128 + 104 * 4 && native_manifest_parse(wire, size, contract, &m, &mf) == NATIVE_MANIFEST_OK);
    CHECK(m.title_id == TITLE_GENERIC && m.program_id == PROGRAM && m.modules[0].module_id == MODULE_ABI_MAIN &&
          m.modules[3].module_id == MODULE_ABI_SDK && m.modules[3].canonical_base == UINT64_C(0x7006000000));
    /* The runtime derives the same adapter from the prepared directory. */
    TitleGenericAdapter g;
    TitleGenericOptions options = {.expected_program_id = PROGRAM, .display_version = "1.2.0"};
    CHECK(title_generic_derive_directory(exefs, &options, &g, why, sizeof why));
    SessionLayoutSpec spec;
    CHECK(title_adapter_build_native_layout_for(&g.adapter, &m, contract, 16384, &spec, NULL) == TITLE_ADAPTER_OK);
    /* Directory rejection: symbolic links and unexpected files. */
    snprintf(path, sizeof path, "%s/link", exefs);
    CHECK(!symlink("main", path));
    CHECK(!title_generic_derive_directory(exefs, &options, &g, why, sizeof why) && strstr(why, "not a regular file"));
    snprintf(manifest, sizeof manifest, "%s/second.manifest", dir);
    CHECK(!native_prepare_exefs("generic", exefs, manifest, contract, shared_sha256_bytes, &result, why, sizeof why));
    CHECK(access(manifest, F_OK) != 0 && result.modules == 0);
    CHECK(!unlink(path));
    /* A specific program ID never prepares as generic. */
    snprintf(path, sizeof path, "%s/main.npdm", exefs);
    File *n = find(&e, "main.npdm");
    p64(n->data + 0x90, title_adapter_find(TITLE_ECHOES)->program_id);
    write_file(path, n->data, n->size);
    CHECK(!native_prepare_exefs("generic", exefs, manifest, contract, shared_sha256_bytes, &result, why, sizeof why));
    CHECK(strstr(why, "specific Raitchi adapter exists"));
}

static void install_tests(void) {
    Exefs e;
    standard(&e, false);
    TitleGenericAdapter g;
    char why[256];
    CHECK(derive(&e, PROGRAM, &g, why));
    CHECK(title_adapter_find(TITLE_GENERIC) == NULL);
    CHECK(title_adapter_select(TITLE_GENERIC) == TITLE_ADAPTER_UNKNOWN && title_adapter_selected() == NULL);
    ModuleAbiProfile unused = {0};
    SessionLayoutSpec spec;
    CHECK(title_adapter_build_layout(TITLE_GENERIC, &unused, 0, 4096, &spec, NULL) == TITLE_ADAPTER_UNKNOWN);
    TitleGenericAdapter wrong = g;
    wrong.adapter.key = "las";
    CHECK(title_adapter_install_generic(&wrong) == TITLE_ADAPTER_PROFILE);
    wrong = g; wrong.adapter.id = TITLE_ECHOES;
    CHECK(title_adapter_install_generic(&wrong) == TITLE_ADAPTER_PROFILE);
    wrong = g; wrong.adapter.capabilities = 1;
    CHECK(title_adapter_install_generic(&wrong) == TITLE_ADAPTER_PROFILE);
    CHECK(title_adapter_install_generic(NULL) == TITLE_ADAPTER_ARGUMENT);
    CHECK(title_adapter_find(TITLE_GENERIC) == NULL);
    /* Install copies and rebinds strings, then the source may disappear. */
    TitleGenericAdapter copy = g;
    CHECK(title_adapter_install_generic(&copy) == TITLE_ADAPTER_OK);
    memset(&copy, 0, sizeof copy);
    memset(&g.version, 'x', sizeof g.version - 1);
    const TitleAdapter *installed = title_adapter_find(TITLE_GENERIC);
    CHECK(installed && installed->id == TITLE_GENERIC && !strcmp(installed->version, "1.2.0"));
    CHECK(!strcmp(installed->save_namespace, "generic-0100aaaabbbb0000-1.2.0") && installed->program_id == PROGRAM);
    CHECK(title_adapter_install_generic(&g) == TITLE_ADAPTER_ALREADY_SELECTED);
    CHECK(title_adapter_build_layout(TITLE_GENERIC, &unused, 0, 4096, &spec, NULL) == TITLE_ADAPTER_AOT_UNSUPPORTED);
    NativeManifest m = manifest_for(installed, &e);
    uint8_t contract[32];
    memset(contract, 0x5a, 32);
    CHECK(title_adapter_build_native_layout(TITLE_GENERIC, &m, contract, 4096, &spec, NULL) == TITLE_ADAPTER_OK);
    /* Selection stays once per process, including for generic. */
    CHECK(title_adapter_select(TITLE_GENERIC) == TITLE_ADAPTER_OK && title_adapter_selected() == installed);
    CHECK(title_adapter_select(TITLE_GENERIC) == TITLE_ADAPTER_ALREADY_SELECTED);
    CHECK(title_adapter_select(TITLE_LAS) == TITLE_ADAPTER_ALREADY_SELECTED && title_adapter_selected() == installed);
    /* Known adapters are unchanged and still found. */
    CHECK(title_adapter_find(TITLE_ECHOES)->program_id == UINT64_C(0x01008cf01baac000));
}

static void install_after_select_test(void) {
    Exefs e;
    standard(&e, false);
    TitleGenericAdapter g;
    char why[256];
    CHECK(derive(&e, PROGRAM, &g, why));
    CHECK(title_adapter_select(TITLE_ECHOES) == TITLE_ADAPTER_OK);
    CHECK(title_adapter_install_generic(&g) == TITLE_ADAPTER_ALREADY_SELECTED && title_adapter_find(TITLE_GENERIC) == NULL);
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    uint8_t contract[32];
    for (unsigned i = 0; i < 32; i++) { unsigned v; CHECK(sscanf(argv[2] + 2 * i, "%2x", &v) == 1); contract[i] = (uint8_t)v; }
    derivation_tests();
    rejection_tests();
    package_tests();
    prepare_tests(argv[1], contract);
    if (getenv("GENERIC_TEST_SELECT_FIRST")) install_after_select_test();
    else install_tests();
    printf("%u generic adapter assertions passed (synthetic NSO/NPDM/PFS0 only)\n", checks);
    return 0;
}
