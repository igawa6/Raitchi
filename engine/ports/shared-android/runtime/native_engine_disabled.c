/* Native-blocks backend placeholder for builds without Unicorn.
 * The default CPU path is continuous NCE, which does not use this backend.
 * Build with -DRAITCHI_NATIVE_BLOCKS=ON to include the Unicorn-based
 * diagnostic backend (Unicorn is GPL-2.0). */
#include <string.h>
#include "native_engine.h"
#include "rt.h"

int native_engine_configure(const NativeEngineHooks *hooks) {
    (void)hooks;
    LOG("[native-engine] native-blocks backend is not included in this build; use continuous-nce\n");
    return -1;
}

void native_engine_run(Cpu *cpu) {
    (void)cpu;
    FATAL("native-blocks backend is not included in this build\n");
}

void native_engine_stats(NativeEngineStats *out) {
    memset(out, 0, sizeof *out);
}
