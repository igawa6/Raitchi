/* Uses the pinned LAS MIT build-cache SHA implementation for file integrity. */
#include "cache_sha256.h"
#include "common_session.h"
#include <cstdio>

static void finish_digest(CacheSha256 &sha, uint8_t out[32]) {
    std::string hex = sha.finish();
    for (unsigned i = 0; i < 32; ++i) {
        auto value = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        out[i] = (uint8_t)((value(hex[i * 2]) << 4) | value(hex[i * 2 + 1]));
    }
}
extern "C" bool shared_sha256_bytes(const void *bytes, size_t length, uint8_t out[32]) {
    if ((!bytes && length) || !out) return false;
    CacheSha256 sha;
    sha.add(bytes, length);
    finish_digest(sha, out);
    return true;
}
extern "C" bool shared_sha256_file(const char *path, uint8_t out[32]) {
    if (!path || !out) return false;
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    CacheSha256 sha;
    uint8_t buffer[65536];
    size_t count;
    while ((count = std::fread(buffer, 1, sizeof buffer, file))) sha.add(buffer, count);
    bool valid = !std::ferror(file);
    if (std::fclose(file)) valid = false;
    if (valid) finish_digest(sha, out);
    return valid;
}
