// Synthetic fixture compiled against real upstream headers. Contains no game code.
#include "gamelib.h"
#include <stdlib.h>

#ifndef GAMELIB_MAX_MODULES
#define GAMELIB_MAX_MODULES 2
#endif
#ifndef GUEST_SHIFT
#define GUEST_SHIFT 0
#endif

__attribute__((visibility("default")))
void probe_metadata(uint64_t out[8]) {
    out[0] = GAMELIB_ABI;
    out[1] = sizeof(Cpu);
    out[2] = sizeof(GameLibTables);
    out[3] = GAMELIB_MAX_MODULES;
    out[4] = GUEST_SHIFT;
    out[5] = offsetof(Cpu, pc);
    out[6] = offsetof(Cpu, v);
    out[7] = _Alignof(Cpu);
}

__attribute__((visibility("default")))
int probe_helpers(void) {
    Cpu *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) return 0;
    uint64_t value = 0;
    uint64_t sum = adds64(cpu, UINT64_MAX, 1);
    int ok = sum == 0 && FLAG_Z(cpu) == 1 && FLAG_C(cpu) == 1 && FLAG_N(cpu) == 0 && FLAG_V(cpu) == 0;
    sum = adds64(cpu, 0x7fffffffffffffffull, 1);
    ok &= sum == 0x8000000000000000ull && FLAG_V(cpu) == 1 && FLAG_N(cpu) == 1;
    ST64((uintptr_t)&value, 0x123456789abcdef0ull);
    ok &= LD64((uintptr_t)&value) == 0x123456789abcdef0ull;
    free(cpu);
    return ok;
}
