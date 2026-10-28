/* Direct generated SDK helper imports; guest pointers are actual host addresses. */
#ifndef SHARED_RECOMP_AOT_LIBC_H
#define SHARED_RECOMP_AOT_LIBC_H
#include "aot_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Trusted readable/writable pointers and counts are required, just like the
 * source SDK calls. These are not guest-memory validation or HLE registry APIs.
 * Copy/set preserve x0 (destination). memcmp/strcmp/strncmp return exact first
 * unsigned-byte difference, sign-extended to the uint64_t x0 representation.
 * bcmp returns exactly0/1. All helpers preserve every register except x0. */
AOT_API void hle_memcpy(Cpu *cpu);  /* overlap-safe, matching pinned SDK semantics */
AOT_API void hle_memmove(Cpu *cpu);
AOT_API void hle_memset(Cpu *cpu);
AOT_API void hle_memcmp(Cpu *cpu);
AOT_API void hle_bcmp(Cpu *cpu);
AOT_API void hle_strlen(Cpu *cpu);
AOT_API void hle_strcmp(Cpu *cpu);
AOT_API void hle_strncmp(Cpu *cpu);
AOT_API void hle_memchr(Cpu *cpu);

#ifdef __cplusplus
}
#endif
#endif
