/* Runtime cache schemas: replay is build-bound; opaque Vulkan data is device-bound. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool shared_gpu_build_identity(bool compiler, uint8_t out[32]);
bool shared_gpu_environment(uint8_t out[32]);
bool shared_gpu_game_identity(uint8_t out[32]);
typedef struct {
    uint32_t vendor, device, driver, api;
    uint8_t uuid[16], environment[32];
} SharedGpuDevice;
typedef struct {
    uint32_t magic, version, abi, api, vendor, device, driver, reserved;
    uint8_t uuid[16], environment[32];
    uint64_t length;
    uint8_t sha256[32];
} SharedGpuCacheMeta;
bool shared_gpu_cache_header(const void *data, size_t length, const SharedGpuDevice *device);
bool shared_gpu_cache_metadata(SharedGpuCacheMeta *out, const void *data, size_t length,
                               const SharedGpuDevice *device);
bool shared_gpu_cache_tag_write(const char *path, const SharedGpuCacheMeta *metadata);
/* migrated reports legacy input; caller writes new metadata only AFTER driver acceptance. */
void *shared_gpu_cache_load(const char *blob_path, const char *tag_path,
                           const SharedGpuDevice *device, const char *legacy_gpu,
                           size_t *length, bool *migrated);
#ifdef __cplusplus
}
#endif
