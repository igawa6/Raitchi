/* Asset-free regression coverage of the extracted SDK helper ABI/semantics. */
#include "runtime/aot_libc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static Cpu cpu;
/* Sentinel-filled full Cpu verifies helpers don't clobber registers, FP state,
 * exclusive state, vector state or the dispatch cache. x0 is the sole exception. */
static void invoke(GuestFn helper, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t want) {
    Cpu before;
    memset(&cpu, 0x5a, sizeof cpu);
    cpu.x[0] = x0; cpu.x[1] = x1; cpu.x[2] = x2;
    memcpy(&before, &cpu, sizeof before);
    helper(&cpu);
    CHECK(cpu.x[0] == want);
    before.x[0] = want;
    CHECK(memcmp(&before, &cpu, sizeof before) == 0);
}
#define PTR(p) ((uint64_t)(uintptr_t)(p))

static void copy_cases(void) {
    GuestFn helpers[] = {hle_memcpy, hle_memmove};
    for (unsigned i = 0; i < sizeof helpers / sizeof *helpers; ++i) {
        unsigned char bytes[10] = "abcdefghi";
        invoke(helpers[i], PTR(bytes + 2), PTR(bytes), 6, PTR(bytes + 2));
        CHECK(memcmp(bytes, "ababcdefi", sizeof bytes) == 0);
        memcpy(bytes, "abcdefghi", sizeof bytes);
        invoke(helpers[i], PTR(bytes), PTR(bytes + 2), 6, PTR(bytes));
        CHECK(memcmp(bytes, "cdefghghi", sizeof bytes) == 0);
        invoke(helpers[i], PTR(bytes), PTR(bytes), sizeof bytes, PTR(bytes));
        CHECK(memcmp(bytes, "cdefghghi", sizeof bytes) == 0);
        unsigned char source[] = {0xf1, 0, 0x81, 0x42};
        unsigned char destination[] = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6};
        invoke(helpers[i], PTR(destination + 1), PTR(source), 0, PTR(destination + 1));
        const unsigned char original[] = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6};
        CHECK(memcmp(destination, original, sizeof destination) == 0);
        invoke(helpers[i], PTR(destination + 1), PTR(source), sizeof source, PTR(destination + 1));
        const unsigned char copied[] = {0xa1, 0xf1, 0, 0x81, 0x42, 0xa6};
        CHECK(memcmp(destination, copied, sizeof destination) == 0);
    }
}
static void memset_cases(void) {
    unsigned char bytes[] = {1, 2, 3, 4, 5, 6};
    invoke(hle_memset, PTR(bytes + 1), 0x1234ab, 4, PTR(bytes + 1));
    const unsigned char want[] = {1, 0xab, 0xab, 0xab, 0xab, 6};
    CHECK(memcmp(bytes, want, sizeof bytes) == 0);
    invoke(hle_memset, PTR(bytes + 2), 0, 0, PTR(bytes + 2));
    CHECK(memcmp(bytes, want, sizeof bytes) == 0);
    invoke(hle_memset, PTR(bytes + 1), UINT64_MAX, 1, PTR(bytes + 1));
    CHECK(bytes[1] == 0xff && bytes[0] == 1 && bytes[2] == 0xab);
}
static void memory_comparison_cases(void) {
    const unsigned char a[] = {0x11, 0, 0xff, 0x7f};
    const unsigned char b[] = {0x11, 0, 0, 0xff};
    invoke(hle_memcmp, PTR(a), PTR(b), 2, 0); /* mismatch beyond bound */
    invoke(hle_memcmp, PTR(a), PTR(b), 3, 255);
    invoke(hle_memcmp, PTR(b), PTR(a), 3, (uint64_t)(int64_t)-255);
    invoke(hle_memcmp, PTR(a), PTR(b), 0, 0);
    invoke(hle_memcmp, PTR(a), PTR(a), sizeof a, 0);
    const unsigned char high[] = {0x80}, low[] = {1};
    invoke(hle_memcmp, PTR(high), PTR(low), 1, 127);
    invoke(hle_memcmp, PTR(low), PTR(high), 1, UINT64_MAX - 126);
    invoke(hle_bcmp, PTR(a), PTR(b), 0, 0);
    invoke(hle_bcmp, PTR(a), PTR(b), 2, 0);
    invoke(hle_bcmp, PTR(a), PTR(a), sizeof a, 0);
    invoke(hle_bcmp, PTR(a), PTR(b), 3, 1);
    invoke(hle_bcmp, PTR(b), PTR(a), 3, 1);
}
static void string_cases(void) {
    const unsigned char empty[] = {0};
    const unsigned char high[] = {0xff, 0}, low[] = {1, 0};
    const unsigned char terminated[] = {'a', 'b', 0, 0xff};
    invoke(hle_strlen, PTR(empty), 0x1234, 0x5678, 0);
    invoke(hle_strlen, PTR(terminated), 0x1234, 0x5678, 2);
    invoke(hle_strcmp, PTR(high), PTR(low), 0, 254);
    invoke(hle_strcmp, PTR(low), PTR(high), 0, (uint64_t)(int64_t)-254);
    invoke(hle_strcmp, PTR(empty), PTR(high), 0, (uint64_t)(int64_t)-255);
    invoke(hle_strcmp, PTR(high), PTR(empty), 0, 255);
    invoke(hle_strcmp, PTR(terminated), PTR("ab"), 0, 0);
    invoke(hle_strcmp, PTR("ab"), PTR("abc"), 0, (uint64_t)(int64_t)-'c');
    /* No NUL exists in these exact-size objects: sanitizer runs detect overread. */
    const unsigned char bounded_a[] = {'p', 0x80, 0xff};
    const unsigned char bounded_b[] = {'p', 0x80, 0};
    invoke(hle_strncmp, PTR(bounded_a), PTR(bounded_b), 0, 0);
    invoke(hle_strncmp, PTR(bounded_a), PTR(bounded_b), 1, 0);
    invoke(hle_strncmp, PTR(bounded_a), PTR(bounded_b), 2, 0);
    invoke(hle_strncmp, PTR(bounded_a), PTR(bounded_b), 3, 255);
    invoke(hle_strncmp, PTR(bounded_b), PTR(bounded_a), 3, (uint64_t)(int64_t)-255);
    invoke(hle_strncmp, PTR(bounded_a), PTR(bounded_a), sizeof bounded_a, 0);
    invoke(hle_strncmp, PTR(high), PTR(low), 1, 254);
    invoke(hle_strncmp, PTR(empty), PTR(high), UINT64_MAX, (uint64_t)(int64_t)-255);
    invoke(hle_strncmp, PTR(terminated), PTR("ab"), UINT64_MAX, 0);
}
static void memchr_cases(void) {
    const unsigned char bytes[] = {0x80, 0, 0xfe, 0x80};
    invoke(hle_memchr, PTR(bytes), 0x80, sizeof bytes, PTR(bytes));
    invoke(hle_memchr, PTR(bytes), 0x1fe, sizeof bytes, PTR(bytes + 2));
    invoke(hle_memchr, PTR(bytes), 0, sizeof bytes, PTR(bytes + 1));
    invoke(hle_memchr, PTR(bytes), 0xfe, 2, 0); /* does not search beyond count */
    invoke(hle_memchr, PTR(bytes), 0xfe, 3, PTR(bytes + 2)); /* scans through NUL */
    invoke(hle_memchr, PTR(bytes), 0x81, sizeof bytes, 0);
    invoke(hle_memchr, PTR(bytes), 0x80, 0, 0);
}
int main(void) {
    copy_cases(); memset_cases(); memory_comparison_cases(); string_cases(); memchr_cases();
    printf("shared AOT libc: %u checks passed\n", checks);
    return 0;
}
