/* Launcher-side package classification and metadata JSON; no JNI, no guest code. */
#include "native_package_metadata.h"
#include "adapters/generic/generic.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *native_package_json_quote(const char *value) {
    if (!value) value = "";
    size_t n = strlen(value);
    if (n > (SIZE_MAX - 3) / 6) return NULL;
    char *out = malloc(n * 6 + 3);
    if (!out) return NULL;
    char *p = out;
    *p++ = '"';
    for (const unsigned char *s = (const unsigned char *)value; *s; s++) {
        if (*s == '"' || *s == '\\') { *p++ = '\\'; *p++ = (char)*s; }
        else if (*s < 32) { snprintf(p, 7, "\\u%04x", *s); p += 6; }
        else *p++ = (char)*s;
    }
    *p++ = '"';
    *p = 0;
    return out;
}

static bool package_list(void *context, TitleGenericVisit visit, void *visit_context) {
    return nx_package_exefs_list(context, visit, visit_context);
}
static bool package_read(void *context, const char *name, uint64_t offset, void *dst, size_t size) {
    return nx_package_exefs_read(context, name, offset, dst, size);
}

void native_package_classify(NxPackage *p, const NxPackageMetadata *m, NativePackageClass *out) {
    memset(out, 0, sizeof *out);
    out->adapter_id = "";
    uint64_t id = nx_package_title_id(p);
    const TitleAdapter *a = NULL;
    for (int i = TITLE_LAS; i <= TITLE_WONDER; i++) {
        const TitleAdapter *c = title_adapter_find((enum TitleId)i);
        if (c && c->program_id == id) { a = c; break; }
    }
    const char *reason = "No Raitchi adapter matches this program ID";
    bool compatible = false;
    if (a) {
        compatible = true;
        reason = a->id == TITLE_WONDER ? "Experimental base 1.0.0 adapter; runtime acceptance pending"
                                       : "Supported adapter and all required module build identities";
        if (!a->main_build_prefix_size || a->main_build_prefix_size > 20) { compatible = false; reason = "Adapter has no valid main build identity"; }
        for (uint32_t i = 0; compatible && i < a->module_count; i++) {
            uint8_t build[32];
            uint32_t prefix = a->module_build_prefix_size[i];
            if (prefix > 20 || !nx_package_exefs_build_id(p, a->modules[i].name, build)) {
                compatible = false;
                snprintf(out->reason, sizeof out->reason, "Required module missing or invalid: %.96s", a->modules[i].name);
                reason = NULL;
            } else if (prefix && memcmp(build, a->module_build_prefix[i], prefix)) {
                compatible = false;
                snprintf(out->reason, sizeof out->reason, "Unsupported build identity: %.96s", a->modules[i].name);
                reason = NULL;
            }
        }
        if (compatible && memcmp(m->main_build_id, a->main_build_prefix, a->main_build_prefix_size)) {
            compatible = false;
            reason = "Unsupported main executable build prefix";
        }
        out->adapter = a;
        out->adapter_id = a->key;
    } else {
        /* Generic startup derives everything from the package; the version only
         * names the save directory. A rejection keeps its exact reason. */
        TitleGenericOptions options = {.expected_program_id = id, .display_version = m ? m->version : NULL};
        TitleGenericSource source = {p, package_list, package_read};
        TitleGenericAdapter generic;
        char why[192];
        if (title_generic_derive(&source, &options, &generic, why, sizeof why)) {
            out->tryable = true;
            out->adapter_id = TITLE_GENERIC_KEY;
            reason = NATIVE_PACKAGE_TRYABLE_REASON;
        } else {
            snprintf(out->reason, sizeof out->reason, "No Raitchi adapter matches this program ID; generic startup cannot try it: %s",
                     why[0] ? why : "unsupported package");
            reason = NULL;
        }
    }
    out->compatible = compatible;
    if (reason) snprintf(out->reason, sizeof out->reason, "%s", reason);
}

char *native_package_metadata_json(uint64_t id, const NxPackageMetadata *m, const char *icon_path,
                                   const NativePackageClass *c) {
    char *qtitle = native_package_json_quote(m->title), *qdev = native_package_json_quote(m->developer),
         *qversion = native_package_json_quote(m->version), *qicon = native_package_json_quote(icon_path),
         *qadapter = native_package_json_quote(c->adapter_id), *qreason = native_package_json_quote(c->reason), *json = NULL;
    if (qtitle && qdev && qversion && qicon && qadapter && qreason) {
        size_t cap = strlen(qtitle) + strlen(qdev) + strlen(qversion) + strlen(qicon) + strlen(qadapter) + strlen(qreason) + 256;
        json = malloc(cap);
        if (json)
            snprintf(json, cap, "{\"title\":%s,\"developer\":%s,\"programId\":\"%llu\",\"programIdHex\":\"%016llx\",\"version\":%s,"
                     "\"iconPath\":%s,\"adapterId\":%s,\"compatible\":%s,\"tryable\":%s,\"reason\":%s}",
                     qtitle, qdev, (unsigned long long)id, (unsigned long long)id, qversion, qicon, qadapter,
                     c->compatible ? "true" : "false", c->tryable ? "true" : "false", qreason);
    }
    free(qtitle); free(qdev); free(qversion); free(qicon); free(qadapter); free(qreason);
    return json;
}
