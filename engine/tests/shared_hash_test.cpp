#include "common_session.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "hash line%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
static std::string hex(const uint8_t digest[32]) {
    char buffer[65];
    for (unsigned i = 0; i < 32; ++i) std::snprintf(buffer + i * 2, 3, "%02x", digest[i]);
    return buffer;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    uint8_t digest[32];
    CHECK(shared_sha256_bytes("", 0, digest));
    CHECK(hex(digest) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(shared_sha256_bytes("abc", 3, digest));
    CHECK(hex(digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string million(1000000, 'a');
    CHECK(shared_sha256_bytes(million.data(), million.size(), digest));
    CHECK(hex(digest) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    std::string path = std::string(argv[1]) + "/hash-fixture-XXXXXX";
    int fd = mkstemp(path.data());
    CHECK(fd >= 0);
    FILE *file = fdopen(fd, "wb");
    CHECK(file && std::fwrite(million.data(), 1, million.size(), file) == million.size());
    CHECK(!std::fclose(file));
    CHECK(shared_sha256_file(path.c_str(), digest));
    CHECK(hex(digest) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(!unlink(path.c_str()));
    CHECK(!shared_sha256_file(path.c_str(), digest));
    CHECK(!shared_sha256_file(nullptr, digest));
    CHECK(!shared_sha256_bytes(nullptr, 1, digest));
    CHECK(!shared_sha256_bytes("abc", 3, nullptr));
    std::printf("shared production SHA wrapper: %u checks passed\n", checks);
    return 0;
}
