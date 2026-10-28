// Call the production HLE entry; substitute only the asynchronous UI boundary.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include SWKBD_SOURCE

static const char *answer, *expected_initial;
static int confirmed = 1, failures, cases;
static int ui_max;
int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    if (strcmp(initial, expected_initial)) {
        fprintf(stderr, "FAIL initial: expected %s, got %s\n", expected_initial, initial);
        failures++;
    }
    ui_max = max_len;
    if (confirmed) snprintf(out, cap, "%s", answer);
    return confirmed;
}
static void put32(uint8_t *p, uint32_t n) { memcpy(p, &n, 4); }
static void run(const char *name, const char *input, size_t cap,
                const uint8_t *expected, size_t expected_size, const uint8_t *initial,
                size_t initial_size, const char *initial_utf8, int max_len) {
    // Guarded buffers, non-zero work offset, config padding and output reused by calls.
    static uint8_t guarded[132];
    uint8_t arg[0x4e0] = {0}, work[256];
    memset(work, 0xab, sizeof work);
    if (initial_size) memcpy(work + 14, initial, initial_size);
    put32(arg + 0x3ac, max_len);
    arg[0x3bc] = 1; arg[0x3bd] = 1; arg[0x3be] = 1;  // use_utf8 set: the SDK argument has no UTF-8 mode
    put32(arg + 0x3c0, 14); put32(arg + 0x3c4, initial_size / 2);
    uint64_t wb = (uintptr_t)work;
    memcpy(arg + 0x4d0, &wb, 8);
    uint8_t saved_arg[sizeof arg]; memcpy(saved_arg, arg, sizeof arg);
    memset(guarded, 0xa5, sizeof guarded);
    uint64_t output[2] = {(uintptr_t)(guarded + 2), cap};
    Cpu cpu = {0}; cpu.x[0] = (uintptr_t)output; cpu.x[1] = (uintptr_t)arg;
    answer = input; expected_initial = initial_utf8;
    int before = failures;
    swkbd_show(&cpu);
    cases++;
    int ok = cpu.x[0] == 0 && output[0] == (uintptr_t)(guarded + 2) && output[1] == cap &&
             !memcmp(arg, saved_arg, sizeof arg) && ui_max == max_len && expected_size <= cap &&
             !memcmp(guarded + 2, expected, expected_size);
    for (size_t i = expected_size; i < cap; i++) ok &= guarded[i + 2] == 0;
    for (size_t i = 0; i < 2; i++) ok &= guarded[i] == 0xa5;
    for (size_t i = cap + 2; i < sizeof guarded; i++) ok &= guarded[i] == 0xa5;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL %s bytes:", name);
        for (size_t i = 0; i < cap; i++) fprintf(stderr, " %02x", guarded[i + 2]);
        fputc('\n', stderr);
    }
    printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}
// UTF-16LE expected bytes, NUL included.
#define U16(...) { __VA_ARGS__, 0, 0 }
#define B(s) (const uint8_t *)(s), sizeof(s) - 1
int main(void) {
    static const uint8_t alex[] = U16('A', 0, 'l', 0, 'e', 0, 'x', 0, 'a', 0, 'n', 0, 'd', 0, 'r', 0, 'e', 0);
    static const uint8_t acc[] = U16(0xc9, 0, 'l', 0, 0xe9, 0, 'o', 0, 'n', 0, 'o', 0, 'r', 0, 'e', 0);
    static const uint8_t acc3[] = U16(0xc9, 0, 'l', 0, 0xe9, 0);
    static const uint8_t sur2[] = U16('A', 0, 0x3d, 0xd8, 0, 0xde);
    static const uint8_t sur1[] = U16('A', 0);
    static const uint8_t acc2[] = U16('A', 0, 'l', 0);
    static const uint8_t bob[] = U16('B', 0, 'o', 0, 'b', 0);
    static const uint8_t abc12[] = U16('A', 0, 'B', 0, 'C', 0, 'D', 0, 'E', 0, 'F', 0, 'G', 0, 'H', 0, 'I', 0,
                                        'J', 0, 'K', 0, 'L', 0);
    static const uint8_t empty[] = {0, 0};
    run("Alexandre UTF16LE", "Alexandre", 32, B(alex), NULL, 0, "", 12);
    run("accent UTF16LE", "\xc3\x89l\xc3\xa9onore", 32, B(acc), NULL, 0, "", 12);
    run("length in UTF16 units", "\xc3\x89l\xc3\xa9onore", 32, B(acc3), NULL, 0, "", 3);
    run("length counts code units not bytes", "\xc3\x89l\xc3\xa9onore", 32, B(acc), NULL, 0, "", 9);
    run("odd byte capacity", "Alexandre", 7, B(acc2), NULL, 0, "", 12);
    run("one byte capacity", "Alexandre", 1, B(empty), NULL, 0, "", 12);
    run("two byte capacity", "Alexandre", 2, B(empty), NULL, 0, "", 12);
    run("three byte capacity", "Alexandre", 3, B(empty), NULL, 0, "", 12);
    run("surrogate exact", "A\xf0\x9f\x98\x80Z", 32, B(sur2), NULL, 0, "", 3);
    run("surrogate boundary", "A\xf0\x9f\x98\x80Z", 32, B(sur1), NULL, 0, "", 2);
    run("initial offset units", "Alexandre", 32, B(alex), B(acc), "\xc3\x89l\xc3\xa9onore", 12);
    confirmed = 0;
    run("cancel preserves initial", "unused", 32, B(acc), B(acc), "\xc3\x89l\xc3\xa9onore", 12);
    run("cancel empty no fallback", "unused", 32, B(empty), NULL, 0, "", 12);
    confirmed = 1;
    run("second shorter entry", "Bob", 32, B(bob), NULL, 0, "", 12);
    run("exact max", "ABCDEFGHIJKLmore", 32, B(abc12), NULL, 0, "", 12);
    printf("%d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
