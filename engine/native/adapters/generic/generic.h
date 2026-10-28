/* Generic title adapter: values derived from one base package's ExeFS.
 *
 * Used only when no specific adapter matches the program ID. Everything that
 * the package states is read from it: the ExeFS module set (main, subsdk0,
 * subsdk1, sdk in standard load order; rtld is not loaded because the shared
 * loader performs its role), NSO segment extents and build IDs, and the NPDM
 * program ID, address space and main-thread values. What a package does not
 * state is a documented assumption (see title_generic_describe()). */
#ifndef SHARED_RECOMP_GENERIC_ADAPTER_H
#define SHARED_RECOMP_GENERIC_ADAPTER_H
#include "../title_adapter.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only view of one ExeFS. list() calls visit once per entry and returns
 * false when the table cannot be read or visit returned false. read() copies
 * exactly size bytes from offset of the named entry. */
typedef bool (*TitleGenericVisit)(void *visit_context, const char *name, uint64_t size);
typedef struct TitleGenericSource {
    void *context;
    bool (*list)(void *context, TitleGenericVisit visit, void *visit_context);
    bool (*read)(void *context, const char *name, uint64_t offset, void *dst, size_t size);
} TitleGenericSource;

typedef struct TitleGenericOptions {
    uint64_t expected_program_id; /* package (NCA) program ID; 0 = not checked */
    const char *display_version;  /* NACP display version; sanitized, "0" if empty */
    /* NVN API version cannot be read from a package. Zero major keeps the
     * legacy 53.207 ABI used by three of the four specific adapters. */
    uint32_t nvn_api_major, nvn_api_minor;
} TitleGenericOptions;

/* Generic preparation policy, shared with the specific adapters' layout. */
#define TITLE_GENERIC_MAPPING_ALIGNMENT 16384u
#define TITLE_GENERIC_MODULE_SPACING UINT64_C(0x2000000)

/* Derive a complete adapter. On failure *out is cleared and why holds a
 * user-facing reason. No title is selected or installed. */
bool title_generic_derive(const TitleGenericSource *source, const TitleGenericOptions *options,
    TitleGenericAdapter *out, char *why, size_t why_size);

/* Same, reading an app-owned extracted ExeFS directory (regular files only,
 * symbolic links are rejected). */
bool title_generic_derive_directory(const char *exefs_dir, const TitleGenericOptions *options,
    TitleGenericAdapter *out, char *why, size_t why_size);

/* NACP display version -> save-namespace token: [A-Za-z0-9._-], other bytes
 * become '_', at most TITLE_GENERIC_VERSION_MAX bytes, "0" when empty.
 * The Android launcher implements the same rule (TitlePolicy.genericVersion). */
void title_generic_version_token(const char *version, char out[TITLE_GENERIC_VERSION_MAX + 1]);

/* One log line naming derived values and every assumed value. */
void title_generic_describe(const TitleGenericAdapter *generic, char *out, size_t size);

/* Parse "MAJOR.MINOR" (both decimal, major 1..999). False leaves outputs zero. */
bool title_generic_parse_nvn_api(const char *text, uint32_t *major, uint32_t *minor);

#ifdef __cplusplus
}
#endif
#endif
