#ifndef SHARED_RECOMP_GUEST_PATH_H
#define SHARED_RECOMP_GUEST_PATH_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Join a canonical guest filesystem path beneath the selected mount root.
 * Leading/repeated slashes and '.' are normalized; '..' is rejected rather
 * than escaping a title's state namespace. No symlink resolution/file access.
 * Mount roots must be runtime-owned directories without guest-created symlinks
 * (the guest filesystem API does not provide a symlink operation). Inputs/output
 * must not alias. Output is empty on failure. */
__attribute__((visibility("default"))) int shared_guest_path(const char *root,
    const char *guest, char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
