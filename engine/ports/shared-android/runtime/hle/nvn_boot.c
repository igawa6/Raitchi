/* Shared NVN proc resolution overlay, based on pinned LAS Recomp Android
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51: runtime/hle/nvn_boot.c.
 * MIT License
 *
 * Copyright (c) 2026 Peiikai
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
// NVN entry points: every NVN function is looked up by name and bound to a host stub.
#include <string.h>

#include "../hle.h"

extern const char *hle_name_at(uint64_t pc);

// This is an explicit diagnostic trap, not an implementation or an API capability.
// Registering it lets a proc-table initializer finish; any actual call remains fatal.
static void nvn_diagnostic_unimplemented(Cpu *c) {
    const char *name = hle_name_at(c->pc);
    FATAL("[nvn-diagnostic] unimplemented API INVOKED: %s (pc=%llx lr=%llx); no implementation or success result\n",
          name ? name : "<unresolved diagnostic entry>", (unsigned long long)c->pc, (unsigned long long)c->x[30]);
}

static uint64_t nvn_proc(const char *name) {
    if (!name || !*name) return 0;
    HostFn implementation = hle_lookup_impl(name);
    const char *diagnostic = getenv("LARECOMP_NVN_DIAGNOSTIC_TRAPS");
    if (!implementation && diagnostic && !strcmp(diagnostic, "1")) {
        LOG("[nvn-diagnostic] unresolved API lookup: %s; diagnostic trap address only, actual invocation is fatal\n", name);
        implementation = nvn_diagnostic_unimplemented;
    }
    // Preserve the strict default: unknown functions fail at resolution.
    return hle_register(name, implementation);
}

HLE_FUNC("nvnBootstrapLoader", nvn_bootstrap_loader) {
    const char *name = (const char *)(uintptr_t)c->x[0];
    c->x[0] = nvn_proc(name);
}

// void* nvnDeviceGetProcAddress(const NVNdevice*, const char*)
HLE_FUNC("nvnDeviceGetProcAddress", nvn_device_get_proc_address) {
    const char *name = (const char *)(uintptr_t)c->x[1];
    c->x[0] = nvn_proc(name);
}
