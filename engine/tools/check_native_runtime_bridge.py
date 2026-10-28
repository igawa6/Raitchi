#!/usr/bin/env python3
"""Build an isolated native->actual rt_svc->native proof. No ADB/game/runtime edits."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from check_native_las_leaf import ROOT, require, run, sha

SOURCE = ROOT / "native/native-runtime-bridge"
RUNNER = '''import ctypes,hashlib,json,os,sys
config=json.load(open(sys.argv[1]));runtime,helper=sys.argv[2:4]
for path,key in ((runtime,'runtime_sha256'),(helper,'helper_sha256')):
    with open(path,'rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
    if digest!=config[key]: raise SystemExit('SHA256 mismatch: '+key)
for key in ('LARECOMP_PROF_SVC','LARECOMP_TRACE_SVC'): os.environ.pop(key,None)
os.environ['LARECOMP_TICK_SCALE']='1'
lib=ctypes.CDLL(helper,mode=ctypes.RTLD_LOCAL);probe=lib.native_runtime_bridge_probe
probe.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t];probe.restype=ctypes.c_int
output=ctypes.create_string_buffer(4096);status=probe(os.fsencode(runtime),output,len(output))
report=json.loads(output.value.decode());report['runtime_sha256']=config['runtime_sha256'];report['helper_sha256']=config['helper_sha256'];print(json.dumps(report),flush=True)
raise SystemExit(status!=0)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-sha256", required=True)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    require(re.fullmatch(r"[0-9a-f]{64}", args.runtime_sha256), "Expected full measured runtime SHA256")
    receipt_path = ROOT / "build/shared-android/native-build.json"
    receipt = json.loads(receipt_path.read_text())
    require(receipt["library_sha256"] == args.runtime_sha256, "Runtime hash differs from authoritative native build")
    pinned = receipt["overlay_and_native_sha256"]
    cpu_header = ROOT / "native/runtime/aot_cpu.h"
    kernel = ROOT / "ports/shared-android/runtime/kernel.c"
    require(sha(cpu_header) == pinned[str(cpu_header.relative_to(ROOT))] and sha(kernel) == pinned[str(kernel.relative_to(ROOT))],
            "Current production CPU header/kernel differ from measured runtime sources")
    require("void rt_svc(Cpu *c, uint32_t imm, uint64_t pc);" in cpu_header.read_text(), "Production API signature changed")
    require(re.search(r"case 0x1E:\s*x\[0\]\s*=\s*rt_ticks\(\);\s*break;", kernel.read_text()), "SystemTick implementation changed")
    platform = ROOT / "work/shared-android/runtime/platform/android.c"
    base_platform = ROOT / receipt["base_source"]["path"] / "runtime/platform/android.c"
    require(sha(platform) == sha(base_platform) and re.search(
        r"uint64_t plat_now_ns\(void\)\s*\{[^}]*clock_gettime\(CLOCK_MONOTONIC,", platform.read_text()),
        "Production platform clock differs from reviewed monotonic implementation")
    memory_dir = ROOT / json.loads((ROOT / "evidence/native-las-memory-current.json").read_text())["directory"]
    memory = json.loads((memory_dir / "stdout.json").read_text())
    require(json.loads((memory_dir / "status.json").read_text())["returncode"] == 0 and memory["state"] == "passed" and
            memory["cases"] == 180 and memory["failures"] == 0, "Previous memory-function gate incomplete")
    require(memory["helper_sha256"] == sha(ROOT / "build/native-las-memory-proof/libnative_las_memory_probe.so"), "Memory gate artifact changed")
    out = ROOT / "build/native-runtime-bridge"; out.mkdir(parents=True, exist_ok=True)
    cc = shutil.which("clang"); require(cc, "Installed host clang required")
    tests = {}
    for name, flags in (("optimized", ["-O2"]), ("sanitized", ["-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = out / ("model-" + name)
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, "-I", ROOT / "native", "-I", SOURCE,
             SOURCE / "model.c", ROOT / "tests/native_runtime_bridge_test.c", "-o", binary], out / (name + "-build.log"))
        text = run([binary], out / (name + "-run.log"))
        tests[name] = {"passed": True, "assertions": int(re.search(r"assertions=(\d+)", text)[1]),
                       "scope": "actual marshal/restore/policy functions with explicitly synthetic unit-test SVC"}
    # Generate assembly constants from the actual production Cpu and this
    # experiment's frame/context definitions. No duplicate numeric Cpu layout.
    generator = out / "offsets.c"
    fields = {"CPU_X": "offsetof(Cpu,x)", "CPU_V": "offsetof(Cpu,v)", "CPU_NZCV": "offsetof(Cpu,nzcv)",
              "CPU_FPCR": "offsetof(Cpu,fpcr)", "CPU_FPSR": "offsetof(Cpu,fpsr)", "NR_FRAME_SIZE": "sizeof(NrFrame)",
              "NR_X": "offsetof(NrFrame,x)", "NR_Q": "offsetof(NrFrame,q)", "NR_SP": "offsetof(NrFrame,sp)",
              "NR_NZCV": "offsetof(NrFrame,nzcv)", "NR_FPCR": "offsetof(NrFrame,fpcr)", "NR_FPSR": "offsetof(NrFrame,fpsr)",
              "NR_CONSUMED": "offsetof(NrFrame,consumed)", "CTX_CPU": "offsetof(NrContext,cpu)",
              "CTX_EXPECTED_SP": "offsetof(NrContext,expected_sp)"}
    generator.write_text('#include "bridge.h"\n#include <stdio.h>\nint main(void) {\n' +
                         "\n".join('printf(".equ ' + key + ', %zu\\n", (size_t)(' + value + '));' for key, value in fields.items()) + '\nreturn 0; }\n')
    emitter = out / "offsets"
    run([cc, "-std=c11", "-I", ROOT / "native", "-I", SOURCE, generator, "-o", emitter], out / "offsets-build.log")
    constants = run([emitter], out / "offsets-run.log")
    (out / "bridge_offsets.inc").write_text(constants)
    require("27.2.12479018" in (args.ndk / "source.properties").read_text(), "Use cached NDK r27c")
    tools = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    # A target compile statically confirms LP64 sizes/alignments used by the
    # host offsetof emitter; mismatch cannot silently cross-link.
    assertions = out / "offset_assertions.c"
    assertions.write_text('#include "bridge.h"\n' + "\n".join(
        '_Static_assert((' + fields[key] + ')== ' + value.strip() + ', "' + key + '");'
        for key, value in re.findall(r"\.equ (\w+),\s*(\d+)", constants)) + '\n')
    run([tools / "aarch64-linux-android30-clang", "-std=c11", "-I", ROOT / "native", "-I", SOURCE,
         "-c", assertions, "-o", out / "offset_assertions.o"], out / "target-offsets.log")
    exports = out / "exports.map"; exports.write_text("NATIVE_RUNTIME_BRIDGE_1 { global: native_runtime_bridge_probe; local: *; };\n")
    library = out / "libnative_runtime_bridge.so"
    run([tools / "aarch64-linux-android30-clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fPIC",
         "-ffixed-x18", "-fvisibility=hidden", "-shared", "-I", ROOT / "native", "-I", SOURCE, "-I", out,
         SOURCE / "model.c", SOURCE / "probe.c", SOURCE / "bridge.S", "-ldl", "-Wl,--no-undefined",
         "-Wl,--version-script=" + str(exports), "-Wl,-z,max-page-size=16384", "-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack",
         "-o", library], out / "android-build.log")
    symbols = run([tools / "llvm-nm", "--dynamic", "--defined-only", library], out / "exports.txt")
    require([line.split()[-1].split("@")[0] for line in symbols.splitlines()] == ["native_runtime_bridge_probe"], "Helper export surface mismatch")
    assembly = run([tools / "llvm-objdump", "--no-show-raw-insn", "-d", library], out / "disassembly.txt")
    bad = []
    for line in assembly.splitlines():
        match = re.match(r"\s*[0-9a-f]+:\s+([a-z0-9.]+)\s+(.*)", line)
        if not match: continue
        op, operands = (s.lower() for s in match.groups())
        if ((op == "msr" and re.match(r"tpidr(?:ro)?_el0\b", operands)) or
            (re.match(r"[xw]18(?:,|$)", operands) and op not in ("str", "stur", "stlr", "stp", "stnp")) or
            (op in ("ldp", "ldnp") and re.match(r"[^,]+,\s*[xw]18\b", operands)) or
            re.search(r"\[x18(?:,[^]]*)?\](?:!|,\s*#)", operands)):
            bad.append((op, operands))
    require(not bad, "Compiled helper writes physical x18/TLS: " + repr(bad))
    config = out / "config.json"
    config.write_text(json.dumps({"schema": 1, "runtime_sha256": args.runtime_sha256, "helper_sha256": sha(library)}, indent=2) + "\n")
    runner = out / "run_bridge.py"; runner.write_text(RUNNER)
    sources = [*sorted(SOURCE.glob("*")), Path(__file__).resolve(), ROOT / "tests/native_runtime_bridge_test.c", cpu_header, kernel,
               ROOT / "tools/check_native_las_leaf.py", platform]
    record = {"schema": 1, "host_tests": tests, "android_cross_link_passed": True, "device_execution": "pending-coordinator",
              "production_runtime_sha256": args.runtime_sha256, "production_build_receipt_sha256": sha(receipt_path),
              "production_api": "native/runtime/aot_cpu.h: rt_svc(Cpu*,uint32_t,uint64_t)", "svc": "SystemTick 0x1e",
              "prior_memory_gate_sha256": sha(memory_dir / "stdout.json"), "generated_offsets_sha256": sha(out / "bridge_offsets.inc"),
              "sources_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sources if p.is_file()},
              "artifacts_sha256": {str(p.relative_to(ROOT)): sha(p) for p in (library, config, runner)},
              "physical_x18_tls_writes": bad,
              "scope": "synthetic native caller, actual production SystemTick, 65 transitions; virtual guestX18/X19; no arbitrary gueststack/SDK/gameboot/exclusive-monitor/performance claim"}
    (out / "verification.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"host_tests": tests, "helper_sha256": sha(library), "android_cross_link_passed": True,
                      "device_execution": record["device_execution"]}, indent=2))


if __name__ == "__main__":
    main()
