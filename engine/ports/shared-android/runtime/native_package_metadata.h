/* Launcher-side package classification and metadata JSON; no JNI, no guest code. */
#ifndef RAITCHI_NATIVE_PACKAGE_METADATA_H
#define RAITCHI_NATIVE_PACKAGE_METADATA_H
#include "nxpkg.h"
#include "adapters/title_adapter.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NATIVE_PACKAGE_TRYABLE_REASON \
    "No Raitchi adapter for this game yet; it can be tried with generic startup and may close unexpectedly"
typedef struct NativePackageClass {
    const TitleAdapter *adapter; /* specific adapter for the program ID, or NULL */
    const char *adapter_id;      /* adapter key, "generic" when tryable, else "" */
    bool compatible;             /* specific adapter and all required build identities */
    bool tryable;                /* no specific adapter; generic startup accepted the package */
    char reason[256];
} NativePackageClass;
/* Specific adapters keep their existing checks and are never tryable. Only an
 * unmatched program ID is checked for the generic adapter, from ExeFS metadata. */
void native_package_classify(NxPackage *package, const NxPackageMetadata *metadata, NativePackageClass *out);
/* Caller frees. NULL on allocation failure. */
char *native_package_metadata_json(uint64_t program_id, const NxPackageMetadata *metadata,
                                   const char *icon_path, const NativePackageClass *classification);
/* JSON string literal with escapes; caller frees. */
char *native_package_json_quote(const char *value);
#ifdef __cplusplus
}
#endif
#endif
