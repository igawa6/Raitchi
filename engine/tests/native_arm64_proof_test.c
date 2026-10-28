#include "proof.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    unsigned checks = 0;
    uint32_t source[3], output[3];
    for (unsigned reg = 0; reg < 32; ++reg) {
        source[0] = 0xd53bd040 | reg; source[1] = 0xd65f03c0;
        memset(output, 0xa5, sizeof output);
        assert((nap_rewrite(source, 2, output) == 0) == (reg < 18)); ++checks;
        if (reg < 18) { assert(output[0] == (0xf9400000 | 19 << 5 | reg)); ++checks; }
        else { assert(output[0] == 0xa5a5a5a5); ++checks; }
        source[0] = 0xaa1203e0 | reg;
        assert((nap_rewrite(source, 2, output) == 0) == (reg < 18)); ++checks;
        if (reg < 18) { assert(output[0] == (0xf9400000 | 1 << 10 | 19 << 5 | reg)); ++checks; }
        source[0] = 0xaa0003f2 | reg << 16;
        assert((nap_rewrite(source, 2, output) == 0) == (reg < 18)); ++checks;
        if (reg < 18) { assert(output[0] == (0xf9000000 | 1 << 10 | 19 << 5 | reg)); ++checks; }
    }
    const uint32_t rejected[] = {0xd503201f, 0xd53bd060, 0x14000000, 0xd4000001, 0xd51bd040, 0xd2800012, 0xf9000260};
    for (size_t i = 0; i < sizeof rejected / sizeof *rejected; ++i) {
        source[0] = 0xd53bd040; source[1] = rejected[i]; source[2] = 0xd65f03c0;
        memset(output, 0xa5, sizeof output);
        assert(nap_rewrite(source, 3, output));
        for (unsigned j = 0; j < 3; ++j) { assert(output[j] == 0xa5a5a5a5); ++checks; }
    }
    source[0] = source[1] = 0xd65f03c0;
    assert(nap_rewrite(source, 2, output)); ++checks;
    assert(nap_rewrite(NULL, 2, output)); ++checks;
    assert(nap_rewrite(source, 2, NULL)); ++checks;
    assert(nap_rewrite(source, 33, output)); ++checks;
    assert(nap_rewrite(source, 1, output)); ++checks;
    puts("whitelist and transactional rejection passed");
    printf("assertions=%u\n", checks);
    return 0;
}
