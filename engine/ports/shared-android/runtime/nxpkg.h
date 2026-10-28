/* Direct package reader adapted from EoWRecomp dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505.
 * Copyright (c) 2026 Peiikai; Copyright (c) 2026 EoWRecomp contributors.
 * MIT: native/runtime/LICENSE-EoWRecomp. No game assets or keys included. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NxPackage NxPackage;
typedef struct NxRomfs NxRomfs;
typedef struct NxPackageMetadata {
    char title[513], developer[257], version[17];
    uint8_t main_build_id[32];
    uint8_t *icon;
    size_t icon_size;
} NxPackageMetadata;
/* Read the owner's seekable NSP/XCI/program NCA. NSZ and patch/delta NCAs are
 * unsupported. The fd variant duplicates fd; callers retain their descriptor.
 * keys_path names prod.keys (optional title.keys is read beside it). Key bytes
 * remain in memory; no decrypted-key descriptor file is written. */
NxPackage *nx_package_open(const char *path, const char *keys_path, char *why, size_t why_size);
NxPackage *nx_package_open_fd(int fd, const char *keys_path, char *why, size_t why_size);
uint64_t nx_package_title_id(const NxPackage *package);
/* Reads real Control NCA NACP fields and the main NSO build ID. The icon is
 * caller-owned and must be released with nx_package_metadata_clear(). */
bool nx_package_read_metadata(NxPackage *package, NxPackageMetadata *metadata, char *why, size_t why_size);
bool nx_package_exefs_build_id(NxPackage *package, const char *name, uint8_t build_id[32]);
/* Metadata-only ExeFS view (no extraction): visit every entry name/size after
 * validating the complete file table, or read exactly size bytes at offset of
 * one named entry. Both reject invalid tables, names and out-of-bounds reads. */
bool nx_package_exefs_list(NxPackage *package, bool (*visit)(void *context, const char *name, uint64_t size), void *context);
bool nx_package_exefs_read(NxPackage *package, const char *name, uint64_t offset, void *dst, size_t size);
void nx_package_metadata_clear(NxPackageMetadata *metadata);
/* Writes only the small original ExeFS files, never the RomFS tree. Existing
 * identical files are accepted; conflicting files are preserved and rejected.
 * Use an app-owned cache directory, not an owner's unrelated files. */
bool nx_package_extract_exefs(NxPackage *package, const char *out_exefs_dir, char *why, size_t why_size);
/* Each reader owns a duplicate fd and decompression cache. Close readers only
 * after their users have joined; read calls on one reader are thread-safe. */
NxRomfs *nx_package_open_romfs(const NxPackage *package, char *why, size_t why_size);
/* Independent descriptor/cache and deep compression metadata. The source must
 * remain alive during cloning; either reader can be closed afterwards. */
NxRomfs *nx_romfs_clone(const NxRomfs *romfs, char *why, size_t why_size);
void nx_package_close(NxPackage *package);
uint64_t nx_romfs_size(const NxRomfs *romfs);
ssize_t nx_romfs_pread(NxRomfs *romfs, void *dst, uint64_t size, uint64_t offset);
void nx_romfs_close(NxRomfs *romfs);
/* Metadata-only, bounded RomFS traversal. No files are extracted. Offsets are
 * absolute in the logical image. Reader and callback contexts remain borrowed;
 * returning false or setting cancel aborts. Complete tree metadata is validated
 * before callbacks. Limits:64MiB metadata,262144 entries,64 levels,4095-byte paths. */
typedef bool (*NxReadAt)(void *context, uint64_t offset, void *dst, size_t size);
typedef bool (*NxRomfsFileVisit)(void *context, const char *path, uint64_t offset, uint64_t size);
bool nx_romfs_visit_reader(NxReadAt read, void *reader, uint64_t image_size,
                          NxRomfsFileVisit visit, void *context, const volatile int *cancel,
                          char *why, size_t why_size);
bool nx_romfs_visit_files(NxRomfs *romfs, NxRomfsFileVisit visit, void *context,
                         const volatile int *cancel, char *why, size_t why_size);

/* Asset-free primitive regression checks. */
void nx_aes128_ecb_decrypt(const uint8_t key[16], uint8_t *data, size_t len);
void nx_aes128_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t len);
int nx_lz4_decompress(const uint8_t *src, int src_len, uint8_t *dst, int dst_len);
#ifdef __cplusplus
}
#endif
