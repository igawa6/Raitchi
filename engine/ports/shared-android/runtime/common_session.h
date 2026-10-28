#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "adapters/title_adapter.h"
#include "runtime/title_manifest.h"
#include "runtime/native_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif
bool shared_title_las(void);
bool shared_renderer_legacy_target_policy(void);
uint32_t shared_input_npad(uint32_t *style);
const TitleManifest *shared_manifest_current(void);
const NativeManifest *shared_native_manifest_current(void);
bool shared_session_ready(void);
uint32_t shared_module_id(uint32_t index);
const ModuleAbiDescriptor *shared_gamelib_descriptor(void);
bool shared_sha256_bytes(const void *bytes, size_t length, uint8_t out[32]);
bool shared_sha256_file(const char *path, uint8_t out[32]);
bool shared_session_prepare(const char *game_root, const char *library,
                            const char *manifest_path, char *why, size_t why_size);
void shared_las_hooks_register(void);
void shared_profiler_initialize(void);
#ifdef __cplusplus
}
#endif
