#!/usr/bin/env python3
"""Asset-free host regression checks of the production NVN keep_interval function.

Extracts the live function unchanged, substitutes controlled clock/VI interfaces,
and runs isolated scenarios with optimization and ASan/UBSan. No device access.
These checks establish deadline/wait behavior, not Android display cadence.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ports/shared-android/runtime/nvn/objects.c"
CASES = ("wall30", "late", "zero", "vi", "vi60", "vi60_selfpaced", "disabled", "wall60", "transition")

PREAMBLE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile uint64_t g_vsync_count;
__thread uint64_t g_blocked_ns;
static uint64_t clock_ns = 1000000000ull;
static uint64_t deadlines[16], vi_targets[16];
static unsigned sleeps, waits, clock_reads;
uint64_t plat_now_ns(void) { ++clock_reads; return clock_ns; }
void plat_sleep_until_ns(uint64_t deadline) {
    assert(sleeps < 16);
    deadlines[sleeps++] = deadline;
    if (deadline > clock_ns) clock_ns = deadline;
}
void vi_wait_vsync(uint64_t count) {
    assert(waits < 16);
    vi_targets[waits++] = count;
    g_vsync_count = count;
    clock_ns += 7000000;
}
'''

SCENARIOS = r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    unsetenv("LARECOMP_PRESENT_INTERVAL");
    const uint64_t start = clock_ns, period = 33333334ull;
    if (!strcmp(argv[1], "wall30")) {
        keep_interval(2);
        assert(!sleeps && !waits && !g_blocked_ns);
        clock_ns += 3000000;
        keep_interval(2);
        assert(sleeps == 1 && deadlines[0] == start + period);
        assert(g_blocked_ns == period - 3000000);
        clock_ns += 5000000;
        keep_interval(2);
        assert(sleeps == 2 && deadlines[1] == start + 2 * period);
        assert(g_blocked_ns == 2 * period - 8000000 && !waits);
    } else if (!strcmp(argv[1], "late")) {
        keep_interval(2);
        clock_ns = start + 5 * period;
        keep_interval(2);
        assert(!sleeps && !waits && !g_blocked_ns);
        clock_ns += 1000000;
        keep_interval(2);
        assert(sleeps == 1 && deadlines[0] == start + 6 * period);
        assert(g_blocked_ns == period - 1000000);
    } else if (!strcmp(argv[1], "zero")) {
        keep_interval(0);
        assert(!clock_reads && !sleeps && !waits);
        keep_interval(2);
        clock_ns += 1000000;
        keep_interval(0);
        assert(clock_reads == 1 && !sleeps && !waits);
        keep_interval(2);  /* interval zero cleared the old deadline */
        assert(!sleeps && !g_blocked_ns);
        keep_interval(2);
        assert(sleeps == 1 && deadlines[0] == start + 1000000 + period);
        assert(g_blocked_ns == period);
    } else if (!strcmp(argv[1], "vi")) {
        g_vsync_count = 10;
        keep_interval(2);
        assert(!sleeps && !waits && !clock_reads);
        g_vsync_count = 11;
        keep_interval(2);
        assert(waits == 1 && vi_targets[0] == 12);
        assert(g_blocked_ns == 7000000 && clock_reads == 2 && !sleeps);
        g_vsync_count = 20;
        keep_interval(2); /* already late: no VI wait */
        g_vsync_count = 21;
        keep_interval(1); /* guest already waited: no additional wait */
        keep_interval(0);
        assert(waits == 1 && !sleeps && g_blocked_ns == 7000000);
    } else if (!strcmp(argv[1], "vi60")) {
        g_vsync_count = 10;
        keep_interval(1);
        assert(!waits && !sleeps && !clock_reads);
        // Timer exists, but no guest wait: every following present must reach
        // the next display tick instead of producing unlimited frames.
        for (unsigned i = 0; i < 8; ++i) {
            keep_interval(1);
            assert(waits == i + 1 && vi_targets[i] == 11 + i);
            assert(g_vsync_count == 11 + i && !sleeps);
        }
        assert(g_blocked_ns == 8 * 7000000ull);
        keep_interval(0);
        keep_interval(0);
        assert(waits == 8); /* interval zero still unpaced with a live timer */
    } else if (!strcmp(argv[1], "vi60_selfpaced")) {
        g_vsync_count = 10;keep_interval(1);
        g_vsync_count = 11;keep_interval(1);
        g_vsync_count = 12;keep_interval(1);
        assert(!waits && !sleeps && !clock_reads && !g_blocked_ns);
        keep_interval(1); /* same tick: the producer outran its own pacing */
        assert(waits == 1 && vi_targets[0] == 13 && g_blocked_ns == 7000000);
        g_vsync_count = 20;keep_interval(1); /* a late frame never adds a wait */
        assert(waits == 1 && !sleeps);
    } else if (!strcmp(argv[1], "disabled")) {
        setenv("LARECOMP_PRESENT_INTERVAL", "0", 1);
        keep_interval(2);
        clock_ns += period;
        keep_interval(2);
        g_vsync_count = 10;
        keep_interval(2);
        g_vsync_count = 11;
        keep_interval(2);
        assert(!clock_reads && !sleeps && !waits && !g_blocked_ns);
    } else if (!strcmp(argv[1], "wall60")) {
        keep_interval(1);
        clock_ns += 2000000;
        keep_interval(1);
        assert(sleeps == 1 && deadlines[0] == start + 16666667ull);
        assert(g_blocked_ns == 14666667ull && !waits);
    } else if (!strcmp(argv[1], "transition")) {
        keep_interval(2);
        g_vsync_count = 100;
        keep_interval(2);
        assert(!sleeps && !waits);
        g_vsync_count = 101;
        keep_interval(2);
        assert(waits == 1 && vi_targets[0] == 102 && !sleeps);
        assert(g_blocked_ns == 7000000);
    } else {
        return 2;
    }
    puts("PASS");
    return 0;
}
'''


def extract(text):
    start = text.index("static void keep_interval(int interval) {")
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth and end < len(text):
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    if depth:
        raise SystemExit("Production keep_interval function is incomplete")
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/shared-present-pacing")
    args = parser.parse_args()
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which(os.environ.get("CC", "clang"))
    if not compiler:
        raise SystemExit("Host clang/CC is required")
    original = SOURCE.read_bytes()
    function = extract(original.decode())
    fixture = output / "present_pacing.c"
    fixture.write_text(PREAMBLE + "\n" + function + "\n" + SCENARIOS)
    results = {}
    for name, flags in (("optimized", ["-O2"]),
                        ("sanitized", ["-O1", "-g", "-fno-omit-frame-pointer",
                                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all"])):
        binary = output / name
        command = [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", *flags,
                   str(fixture), "-o", str(binary)]
        completed = subprocess.run(command, capture_output=True, text=True)
        (output / f"{name}-compile.log").write_text(completed.stdout + completed.stderr)
        if completed.returncode:
            raise SystemExit(f"{name} compilation failed:\n{completed.stderr}")
        for case in CASES:
            completed = subprocess.run([str(binary), case], capture_output=True, text=True)
            (output / f"{name}-{case}.log").write_text(completed.stdout + completed.stderr)
            if completed.returncode:
                raise SystemExit(f"{name}/{case} failed:\n{completed.stdout}{completed.stderr}")
        results[name] = {"command": command, "passed": list(CASES)}
        print(f"{name}: PASS {len(CASES)} controlled pacing scenarios")
    if SOURCE.read_bytes() != original:
        raise SystemExit("Production source changed during checks; rerun")
    (output / "verification.json").write_text(json.dumps({
        "scope": "actual extracted function; controlled host clock/VI, no Android cadence proof",
        "source_sha256": hashlib.sha256(original).hexdigest(),
        "function_sha256": hashlib.sha256(function.encode()).hexdigest(),
        "compiler": compiler, "variants": results,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
