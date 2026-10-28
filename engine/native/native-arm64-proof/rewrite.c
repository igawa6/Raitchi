#include "proof.h"
#include <string.h>

int nap_rewrite(const uint32_t *input, size_t count, uint32_t *output) {
    uint32_t staged[32];
    if (!input || !output || count < 2 || count > 32 || input[count - 1] != 0xd65f03c0u) return -1;
    for (size_t i = 0; i < count; ++i) {
        uint32_t w = input[i], rt = w & 31u;
        if ((w & 0xffffffe0u) == 0xd53bd040u && rt < 18) {
            staged[i] = 0xf9400000u | (19u << 5) | rt; /* guest TLS */
        } else if ((w & 0xffffffe0u) == (0xaa0003e0u | (18u << 16)) && rt < 18) {
            staged[i] = 0xf9400000u | (1u << 10) | (19u << 5) | rt;
        } else if ((w & 0xffe0ffffu) == (0xaa0003e0u | 18u) && ((w >> 16) & 31u) < 18) {
            staged[i] = 0xf9000000u | (1u << 10) | (19u << 5) | ((w >> 16) & 31u);
        } else if (w == 0x8b0a0120u) { /* ADD X0,X9,X10 */
            staged[i] = w;
        } else if (w == 0xd65f03c0u && i == count - 1) {
            staged[i] = w;
        } else return -1;
    }
    memcpy(output, staged, count * sizeof *output);
    return 0;
}
