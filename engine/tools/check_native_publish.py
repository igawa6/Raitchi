#!/usr/bin/env python3
"""Host-only tests of the actual native_prepare.c publication functions.

Adapted from ptichalouf, Raitchi PR #2, commit
51041c7d790a815f75b963efe9251fb233cdce40.

Run: python3 tools/check_native_publish.py
Requires Linux, Python 3 and a C compiler (CC, default clang). Only synthetic bytes
are used. --source accepts an older native_prepare.c for red/green comparisons.
The production functions are extracted verbatim, not reimplemented in the test.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

SUPPORT = r'''
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dirent.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif
#define NATIVE_MANIFEST_MAX_SIZE (128u << 10)
static int forced_errno, deny_link, racing, injected, syscall_calls, link_calls;
static const char *target;
static void put(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text)) abort();
    if (close(fd)) abort();
}
static void inject(void) {
    if (racing && !injected) { put(target, "VICTIM"); injected = 1; }
}
static long publish_syscall(long n, int a, const char *b, int c,
                            const char *d, unsigned flags) {
    syscall_calls++;
    if (forced_errno) { errno = forced_errno; return -1; }
    inject();
    return syscall(n, a, b, c, d, flags);
}
static int publish_link(const char *from, const char *to) {
    link_calls++;
    inject();
    if (deny_link) { errno = EACCES; return -1; }
    return link(from, to);
}
/* Also detects the unsafe historical lstat+rename intermediate version:
   inject AFTER the real absent observation but BEFORE the caller's rename. */
static int publish_lstat(const char *path, struct stat *st) {
    int r = lstat(path, st), e = errno;
    if (r && e == ENOENT) inject();
    errno = e;
    return r;
}
#define syscall publish_syscall
#define link publish_link
#define lstat publish_lstat
'''
MAIN = r'''
#undef syscall
#undef link
#undef lstat
static int contents(const char *path, const char *wanted) {
    char bytes[32] = {0};
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t n = read(fd, bytes, sizeof bytes);
    close(fd);
    return n == (ssize_t)strlen(wanted) && !memcmp(bytes, wanted, (size_t)n);
}
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s; why=%s\n", __LINE__, #c, why); return 1; } } while (0)
int main(int argc, char **argv) {
    if (argc != 6) return 2;
    const char *scenario = argv[2];
    forced_errno = atoi(argv[3]); deny_link = atoi(argv[4]);
    int primary_expected = atoi(argv[5]);
    char path[4096], other[4096], why[256] = {0};
    snprintf(path, sizeof path, "%s/manifest", argv[1]); target = path;
    snprintf(other, sizeof other, "%s/other", argv[1]);
    if (!strcmp(scenario, "same")) put(path, "AAA");
    if (!strcmp(scenario, "conflict")) put(path, "VICTIM");
    if (!strcmp(scenario, "symlink")) {
        put(other, "AAA"); CHECK(!symlink("other", path));
    }
    racing = !strcmp(scenario, "race");
    bool created = false;
    bool ok = publish(path, (const uint8_t *)"AAA", 3, &created, why, sizeof why);
    if (racing) {
        printf("RACE injected=%d victim_intact=%d ok=%d created=%d\n",
               injected, contents(path, "VICTIM"), ok, created);
        CHECK(injected); CHECK(contents(path, "VICTIM"));
    }
    if (!strcmp(scenario, "unavailable")) {
        CHECK(!ok); CHECK(!created); CHECK(access(path, F_OK) == -1 && errno == ENOENT);
        CHECK(!strcmp(why, "Cannot publish atomically without replacement; target left untouched"));
    } else if (!strcmp(scenario, "conflict") || racing || !strcmp(scenario, "symlink")) {
        CHECK(!ok); CHECK(!created);
        CHECK(!strcmp(why, "Conflicting native metadata exists; original preserved"));
        if (racing) CHECK(injected);
        if (!strcmp(scenario, "symlink")) {
            struct stat st; CHECK(!lstat(path, &st) && S_ISLNK(st.st_mode));
            CHECK(contents(other, "AAA"));
        } else CHECK(contents(path, "VICTIM"));
    } else {
        CHECK(ok); CHECK(created == !!strcmp(scenario, "same")); CHECK(contents(path, "AAA"));
    }
    if (primary_expected) { CHECK(syscall_calls == 1); CHECK(link_calls == 0); }
    DIR *dir = opendir(argv[1]); CHECK(dir != NULL);
    struct dirent *entry;
    while ((entry = readdir(dir))) CHECK(!strstr(entry->d_name, ".tmp-"));
    closedir(dir);
    printf("PASS %s errno=%d deny_link=%d injected=%d syscall=%d link=%d\n",
           scenario, forced_errno, deny_link, injected, syscall_calls, link_calls);
    return 0;
}
'''


def function(text, name):
    # Only these small leaf functions are selected; do not copy title/game code.
    match = re.search(r"^static [^\n{]*\b" + re.escape(name) + r"\(", text, re.M)
    if not match:
        raise ValueError("Missing function: " + name)
    start = text.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] / "native/runtime/native_prepare.c")
    args = parser.parse_args()
    source = args.source.read_text()
    functions = "\n\n".join(function(source, name) for name in
                             ("problem", "read_all", "write_all", "same_file", "publish"))
    with tempfile.TemporaryDirectory(prefix="native-publish-", dir=os.environ.get("TMPDIR")) as build:
        build = Path(build)
        cfile = build / "test.c"
        binary = build / "test"
        cfile.write_text(SUPPORT + functions + MAIN)
        # Historical sources do not call every shim. Keep all other warnings fatal.
        subprocess.run(shlex.split(os.environ.get("CC", "clang")) +
                       ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-O0", str(cfile), "-o", str(binary)], check=True)

        class Publication(unittest.TestCase):
            pass

        def case(scenario, error, denied, primary):
            def test(self):
                with tempfile.TemporaryDirectory(prefix="case-", dir=build) as directory:
                    result = subprocess.run([str(binary), directory, scenario, str(error),
                                             str(denied), str(int(primary))],
                                            text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    print(result.stdout.strip(), flush=True)
            return test

        # Real kernel renameat2, with link forbidden as on restricted app domains.
        for scenario in ("create", "same", "conflict", "symlink", "race"):
            setattr(Publication, "test_primary_" + scenario, case(scenario, 0, 1, True))
        # Both unsupported-syscall outcomes; atomic link fallback if permitted.
        for error in (22, 38):  # Linux EINVAL, ENOSYS
            for scenario in ("create", "same", "conflict", "symlink", "race"):
                setattr(Publication, f"test_fallback_{error}_{scenario}", case(scenario, error, 0, False))
            setattr(Publication, f"test_unavailable_{error}", case("unavailable", error, 1, False))
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Publication))
        return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
