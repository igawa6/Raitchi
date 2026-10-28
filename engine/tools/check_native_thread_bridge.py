#!/usr/bin/env python3
"""Build an isolated real-KThread native/TLS/SVC proof. No ADB/game/runtime edits."""
import argparse
import hashlib
import struct
import subprocess
import json
from pathlib import Path
import re
import shutil

from check_native_las_leaf import ROOT, require, run, sha

SOURCE = ROOT / "native/native-thread-bridge"
RUNNER = '''import ctypes,hashlib,json,os,struct,sys
config=json.load(open(sys.argv[1]));runtime,helper,root,game,sidecar=sys.argv[2:7]
extras=sys.argv[7:];stack_mode='--guest-stack' in extras
if extras.count('--guest-stack')>1: raise SystemExit('Duplicate guest-stack flag')
extras=[x for x in extras if x!='--guest-stack']
if len(extras)>1: raise SystemExit('Expected config runtime helper root game sidecar [bundle] [--guest-stack]')
for path,key in ((runtime,'runtime_sha256'),(helper,'helper_sha256'),(game,'game_sha256')):
    with open(path,'rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
    if digest!=config[key]: raise SystemExit('SHA256 mismatch: '+key)
class SdkInput(ctypes.Structure):
    _fields_=[('struct_size',ctypes.c_uint32),('version',ctypes.c_uint32),('words',ctypes.c_uint32*3),('reserved',ctypes.c_uint32),('original_delta',ctypes.c_int64),('aot_address',ctypes.c_uint64)]
sdk=None
if extras:
    info=config.get('sdk')
    if not info: raise SystemExit('SDK mode lacks audited config')
    bundle=open(extras[0],'rb').read()
    if len(bundle)!=12 or hashlib.sha256(bundle).hexdigest()!=info['bundle_sha256']: raise SystemExit('SDK bundle mismatch')
    if hashlib.sha256(bundle[:4]).hexdigest()!=info['wrapper_sha256'] or hashlib.sha256(bundle[4:]).hexdigest()!=info['thunk_sha256']: raise SystemExit('SDK wrapper/thunk mismatch')
    with open(os.path.join(root,'exefs','sdk'),'rb') as f: original_digest=hashlib.file_digest(f,'sha256').hexdigest()
    if original_digest!=info['original_nso_sha256']: raise SystemExit('SDK original differs from candidate')
    sdk=SdkInput(ctypes.sizeof(SdkInput),1,(ctypes.c_uint32*3)(*struct.unpack('<III',bundle)),0,info['original_delta'],info['aot_address'])
for key in ('LARECOMP_PROF_SVC','LARECOMP_TRACE_SVC'): os.environ.pop(key,None)
os.environ['LARECOMP_TICK_SCALE']='1'
lib=ctypes.CDLL(helper,mode=ctypes.RTLD_LOCAL);probe=lib.native_thread_bridge_probe
probe.argtypes=[ctypes.c_char_p]*4+[ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(SdkInput),ctypes.c_uint32];probe.restype=ctypes.c_int
output=ctypes.create_string_buffer(4096);status=probe(*map(os.fsencode,(runtime,root,game,sidecar)),output,len(output),ctypes.byref(sdk) if sdk else None,int(stack_mode))
report=json.loads(output.value.decode());report.update({k:config[k] for k in ('runtime_sha256','helper_sha256','game_sha256')});report['sdk_mode_requested']=sdk is not None;report['guest_stack_requested']=stack_mode
if sdk: report['sdk_bundle_sha256']=config['sdk']['bundle_sha256'];report['sdk_candidate_sha256']=config['sdk']['candidate_sha256']
print(json.dumps(report),flush=True)
raise SystemExit(status!=0)
'''




def sdk_candidate_config(path, game_receipt_path, game_sha256):
    # All original bytes, addresses and generated excerpts remain ignored.
    from audit_game_inputs import load_parser
    require(subprocess.run(["git", "check-ignore", "-q", str(path)], cwd=ROOT).returncode == 0, "SDK candidate must stay ignored")
    candidate = json.loads(path.read_text())
    require(candidate["schema"] == 1 and candidate["state"] == "audited-not-device-executed", "Unaudited SDK candidate")
    auditor = ROOT / "tools/audit_native_sdk_thunk.py"
    require(sha(auditor) == candidate["script_sha256"], "Independent SDK audit script changed")
    original, wrapper, thunk, aot = (candidate[key] for key in ("original", "wrapper", "thunk", "aot"))
    original_path = Path(original["path"])
    require(sha(original_path) == original["sha256"], "Original SDK hash changed")
    Nso, _, parser = load_parser()
    require(sha(parser) == candidate["parser_sha256"], "Pinned original parser changed")
    nso = Nso(original_path)
    text = bytes(nso.image[nso.text[0]:sum(nso.text)])
    require(hashlib.sha256(text).hexdigest() == original["decompressed_text_sha256"], "Original SDK text changed")
    code = []
    for part, size in ((wrapper, 4), (thunk, 8)):
        offset = part["offset"]
        require(part["size"] == size and not offset % 4 and nso.text[0] <= offset and offset + size <= sum(nso.text), "SDK code bounds/alignment rejected")
        source = bytes(nso.image[offset:offset + size])
        require(hashlib.sha256(source).hexdigest() == part["code_sha256"], "SDK source code changed")
        require(not any(site < offset + size and site + 8 > offset for site, *_ in nso.relocs), "SDK code overlaps relocation write")
        code.append(source)
    words = struct.unpack("<III", b"".join(code))
    immediate = words[0] & 0x03ffffff
    delta = (immediate - (1 << 26) if immediate & (1 << 25) else immediate) * 4
    require(words[0] & 0xfc000000 == 0x14000000 and words[1:] == (0xd4000201, 0xd65f03c0) and
            delta == thunk["offset"] - wrapper["offset"] and delta not in (0, -4), "SDK shape is not reviewed B -> SVC10;RET")
    bundle = Path(candidate["bundle"]["path"])
    require(subprocess.run(["git", "check-ignore", "-q", str(bundle)], cwd=ROOT).returncode == 0, "Original bundle must stay ignored")
    require(bundle.read_bytes() == b"".join(code) and sha(bundle) == candidate["bundle"]["sha256"], "SDK bundle differs from original")
    prepare_path = Path(aot["prepare_path"])
    require(sha(prepare_path) == aot["prepare_sha256"], "AOT preparation changed")
    prepare = json.loads(prepare_path.read_text())
    semantic = prepare["semantic"]
    sdk = semantic["modules"]["sdk"]
    require(prepare["semantic_code_sha256"] == aot["semantic_code_sha256"] and sdk["module_id"] == 2 and
            sdk["sha256"] == original["sha256"] and semantic["cpu_identity"] == 3, "AOT SDK identity mismatch")
    expected_address = sdk["canonical_base"] + wrapper["offset"] - semantic["guest_shift"]
    require(expected_address == aot["actual_address"] == wrapper["actual_cpu3_address"], "AOT wrapper address mismatch")
    table = Path(aot["table_path"])
    require(sha(table) == aot["table_sha256"], "AOT table changed")
    table_text = table.read_text()
    address_match = re.search(r"const uint64_t sdk_fn_addrs\[\]\s*=\s*\{(.*?)\};", table_text, re.S)
    function_match = re.search(r"const GuestFn sdk_fn_ptrs\[\]\s*=\s*\{(.*?)\};", table_text, re.S)
    require(address_match and function_match, "AOT table declaration changed")
    addresses = [int(x,16) for x in re.findall(r"0x([0-9a-fA-F]+)ull", address_match[1])]
    functions = re.findall(r"\bf_[0-9a-fA-F]+\b", function_match[1])
    require(len(addresses) == len(functions) == sdk["function_count"], "AOT table length changed")
    for name, part in (("wrapper", wrapper), ("thunk", thunk)):
        item = aot[name]; unit = Path(item["definition_unit"]); index = item["table_index"]
        require(sha(unit) == item["definition_unit_sha256"] and item["definition_excerpt"] in unit.read_text(), "Audited AOT body changed")
        require(addresses[index] == part["actual_cpu3_address"] == item["actual_address"] and functions[index] == item["function_name"], "Audited AOT slot changed")
    require("__attribute__((musttail)) return " + aot["thunk"]["function_name"] + "(c);" in aot["wrapper"]["definition_excerpt"] and
            "rt_svc(c, 16, " + hex(thunk["actual_cpu3_address"]) + "ull);" in aot["thunk"]["definition_excerpt"], "AOT wrapper/thunk behavior changed")
    require(subprocess.run(["git", "check-ignore", "-q", str(game_receipt_path)], cwd=ROOT).returncode == 0, "Game build receipt must stay ignored")
    game_receipt = json.loads(game_receipt_path.read_text())
    produced = game_receipt["semantic"]
    produced_fingerprint = hashlib.sha256(json.dumps(produced, sort_keys=True).encode()).hexdigest()
    require(game_receipt["schema"] == 1 and game_receipt["library_sha256"] == game_sha256 and
            produced_fingerprint == game_receipt["semantic_code_sha256"], "Actual game producer receipt/hash rejected")
    require(game_receipt["title"] == produced["title"] == semantic["title"] == "las" and
            produced["cpu_identity"] == semantic["cpu_identity"] == 3 and
            produced["guest_shift"] == semantic["guest_shift"] and
            produced["execution_abi"] == semantic["execution_abi"] == "shared-aot-v1" and
            produced["contract"] == semantic["contract"], "Actual producer title/CPU/layout/contract differs from audit")
    produced_sdk = produced["modules"]["sdk"]
    for key in ("module_id", "canonical_base", "mapped_size", "text_offset", "text_size", "function_count", "sha256", "build_id"):
        require(produced_sdk[key] == sdk[key], "Actual producer SDK identity differs: " + key)
    generated = Path(prepare["generated_directory"])
    audited_units = {str(table.relative_to(generated)): (table, aot["table_sha256"])}
    for item in (aot["wrapper"], aot["thunk"]):
        unit = Path(item["definition_unit"])
        audited_units[str(unit.relative_to(generated))] = (unit, item["definition_unit_sha256"])
    bound_units = {}
    for name, (unit, expected_hash) in audited_units.items():
        require(sha(unit) == expected_hash == produced["generated_sha256"][name] == semantic["generated_sha256"][name],
                "Actual game generated source differs from audited unit: " + name)
        semantic_objects = [item for item in produced["objects"] if item["unit"] == name]
        linked_objects = [item for item in game_receipt["objects"] if item["unit"] == name]
        require(len(semantic_objects) == len(linked_objects) == 1, "Actual game object record is missing/ambiguous: " + name)
        semantic_object, linked_object = semantic_objects[0], linked_objects[0]
        for key in ("key", "sha256"):
            value = semantic_object[key]
            require(re.fullmatch(r"[0-9a-f]{64}", value) and value != "0" * 64 and linked_object[key] == value,
                    "Actual game object fingerprint invalid: " + name)
        require(bool(linked_object["object"]), "Actual game linked object path is blank: " + name)
        bound_units[name] = {"source_sha256": expected_hash, "object_key": semantic_object["key"], "object_sha256": semantic_object["sha256"]}
    actual_producer = {"receipt_sha256": sha(game_receipt_path), "library_sha256": game_sha256,
                       "semantic_code_sha256": produced_fingerprint, "audited_generated_units": bound_units}
    return {"candidate_sha256": sha(path), "bundle_sha256": sha(bundle), "original_nso_sha256": original["sha256"],
            "wrapper_sha256": wrapper["code_sha256"], "thunk_sha256": thunk["code_sha256"],
            "wrapper_offset": wrapper["offset"], "thunk_offset": thunk["offset"], "original_delta": delta,
            "aot_address": expected_address, "actual_game_producer": actual_producer, "aot_prepare_sha256": sha(prepare_path), "aot_table_sha256": sha(table),
            "method": candidate["method"], "auditor_sha256": sha(auditor), "native_copy_layout": {"wrapper_offset": 2048, "thunk_offset": 2056},
            "native_svc_pc": "owned copied thunk PC, not original SDK architectural PC"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-sha256", required=True)
    parser.add_argument("--sdk-candidate", type=Path)
    parser.add_argument("--game-receipt", type=Path)
    parser.add_argument("--game-sha256", required=True)
    parser.add_argument("--runtime-receipt", type=Path, default=ROOT / "build/shared-android/native-build.json")
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    require(re.fullmatch(r"[0-9a-f]{64}", args.runtime_sha256), "Expected full measured runtime SHA256")
    receipt_path = args.runtime_receipt
    receipt = json.loads(receipt_path.read_text())
    require(receipt["library_sha256"] == args.runtime_sha256, "Runtime hash differs from authoritative native build")
    pinned = receipt["overlay_and_native_sha256"]
    cpu_header = ROOT / "native/runtime/aot_cpu.h"
    kernel = ROOT / "ports/shared-android/runtime/kernel.c"
    require(sha(cpu_header) == pinned[str(cpu_header.relative_to(ROOT))] and sha(kernel) == pinned[str(kernel.relative_to(ROOT))],
            "Current production CPU header/kernel differ from measured runtime sources")
    require("void rt_svc(Cpu *c, uint32_t imm, uint64_t pc);" in cpu_header.read_text(), "Production API signature changed")
    guest_sources = [ROOT / "ports/shared-android/runtime" / name for name in ("guest_session.c", "guest_session.h", "kernel.h", "gamelib.c")]
    for path in guest_sources:
        require(sha(path) == pinned[str(path.relative_to(ROOT))], "Production lifecycle source differs from receipt: " + str(path))
    require(re.fullmatch(r"[0-9a-f]{64}", args.game_sha256), "Expected full game-library hash")
    require(bool(args.sdk_candidate) == bool(args.game_receipt), "SDK mode requires both --sdk-candidate and --game-receipt")
    sdk_config = sdk_candidate_config(args.sdk_candidate, args.game_receipt, args.game_sha256) if args.sdk_candidate else None
    out = ROOT / "build/native-thread-bridge"; out.mkdir(parents=True, exist_ok=True)
    cc = shutil.which("clang"); require(cc, "Installed host clang required")
    tests = {}
    for name, flags in (("optimized", ["-O2"]), ("sanitized", ["-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = out / ("model-" + name)
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, "-I", ROOT / "native", "-I", SOURCE, "-I", ROOT / "ports/shared-android/runtime",
             SOURCE / "model.c", ROOT / "tests/native_thread_bridge_test.c", "-o", binary], out / (name + "-build.log"))
        text = run([binary], out / (name + "-run.log"))
        tests[name] = {"passed": True, "assertions": int(re.search(r"assertions=(\d+)", text)[1]),
                       "scope": "actual marshal/restore/policy functions with explicitly synthetic unit-test SVC"}
    # Generate assembly constants from the actual production Cpu and this
    # experiment's frame/context definitions. No duplicate numeric Cpu layout.
    generator = out / "offsets.c"
    fields = {"CPU_X": "offsetof(Cpu,x)", "CPU_V": "offsetof(Cpu,v)", "CPU_NZCV": "offsetof(Cpu,nzcv)",
              "CPU_FPCR": "offsetof(Cpu,fpcr)", "CPU_FPSR": "offsetof(Cpu,fpsr)", "NT_FRAME_SIZE": "sizeof(NtFrame)",
              "NT_X": "offsetof(NtFrame,x)", "NT_Q": "offsetof(NtFrame,q)", "NT_SP": "offsetof(NtFrame,sp)",
              "NT_NZCV": "offsetof(NtFrame,nzcv)", "NT_FPCR": "offsetof(NtFrame,fpcr)", "NT_FPSR": "offsetof(NtFrame,fpsr)",
              "CTX_CPU": "offsetof(NtContext,cpu)",
              "CTX_EXPECTED_SP": "offsetof(NtContext,expected_sp)", "CTX_ENTRY": "offsetof(NtContext,entry)", "CTX_CALL": "offsetof(NtContext,call)",
              "CTX_HOST_SP": "offsetof(NtContext,host_sp)", "CTX_HOST_CALL_SP": "offsetof(NtContext,host_call_sp)",
              "CTX_ACTIVE_FRAME": "offsetof(NtContext,active_frame)", "CTX_RETURN_SP": "offsetof(NtContext,return_sp)",
              "CTX_GUEST_ENTRY_SP": "offsetof(NtContext,guest_entry_sp)", "CTX_STACK_ENABLED": "offsetof(NtContext,stack_enabled)",
              "CTX_ENTRY_DEPTH": "offsetof(NtContext,entry_depth)", "CTX_GATEWAY_ACTIVE": "offsetof(NtContext,gateway_active)",
              "CTX_ENTRY_REJECTIONS": "offsetof(NtContext,entry_rejections)"}
    generator.write_text('#include "bridge.h"\n#include <stdio.h>\nint main(void) {\n' +
                         "\n".join('printf(".equ ' + key + ', %zu\\n", (size_t)(' + value + '));' for key, value in fields.items()) + '\nreturn 0; }\n')
    emitter = out / "offsets"
    run([cc, "-std=c11", "-I", ROOT / "native", "-I", SOURCE, "-I", ROOT / "ports/shared-android/runtime", generator, "-o", emitter], out / "offsets-build.log")
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
    run([tools / "aarch64-linux-android30-clang", "-std=c11", "-I", ROOT / "native", "-I", SOURCE, "-I", ROOT / "ports/shared-android/runtime",
         "-c", assertions, "-o", out / "offset_assertions.o"], out / "target-offsets.log")
    exports = out / "exports.map"; exports.write_text("NATIVE_THREAD_BRIDGE_1 { global: native_thread_bridge_probe; local: *; };\n")
    library = out / "libnative_thread_bridge.so"
    run([tools / "aarch64-linux-android30-clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fPIC",
         "-ffixed-x18", "-fvisibility=hidden", "-shared", "-I", ROOT / "native", "-I", SOURCE, "-I", ROOT / "ports/shared-android/runtime", "-I", out,
         SOURCE / "model.c", SOURCE / "probe.c", SOURCE / "bridge.S", "-ldl", "-pthread", "-Wl,--no-undefined",
         "-Wl,--version-script=" + str(exports), "-Wl,-z,max-page-size=16384", "-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack",
         "-o", library], out / "android-build.log")
    symbols = run([tools / "llvm-nm", "--dynamic", "--defined-only", library], out / "exports.txt")
    require([line.split()[-1].split("@")[0] for line in symbols.splitlines()] == ["native_thread_bridge_probe"], "Helper export surface mismatch")
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
    config.write_text(json.dumps({"schema": 1, "runtime_sha256": args.runtime_sha256, "helper_sha256": sha(library), "game_sha256": args.game_sha256, "outer_process_timeout_seconds": 30, "guarded_guest_stack_supported": True, "sdk": sdk_config}, indent=2) + "\n")
    runner = out / "run_bridge.py"; runner.write_text(RUNNER)
    sources = [*sorted(SOURCE.glob("*")), Path(__file__).resolve(), ROOT / "tests/native_thread_bridge_test.c", cpu_header, kernel,
               ROOT / "tools/check_native_las_leaf.py", ROOT / "native/module_abi.h", *guest_sources]
    if sdk_config:
        sources += [ROOT / "tools/audit_native_sdk_thunk.py", ROOT / "tools/audit_game_inputs.py"]
    record = {"schema": 1, "host_tests": tests, "android_cross_link_passed": True, "device_execution": "pending-coordinator",
              "production_runtime_sha256": args.runtime_sha256, "production_build_receipt_sha256": sha(receipt_path),
              "production_api": "native/runtime/aot_cpu.h: rt_svc(Cpu*,uint32_t,uint64_t)", "svc": "GetCurrentProcessorNumber 10, GetThreadId 25, SystemTick 1e, ExitThread 0a", "generated_offsets_sha256": sha(out / "bridge_offsets.inc"),
              "sources_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sources if p.is_file()},
              "artifacts_sha256": {str(p.relative_to(ROOT)): sha(p) for p in (library, config, runner)},
              "physical_x18_tls_writes": bad,
              "sdk_mode_audited": sdk_config,
              "scope": "two real production KThreads, synthetic native whitelist and optional original SDK B/SVC10/RET, actual TLS and stateful SVCs; host-backed SP by default or explicit guarded guest stack/host C transition, virtual guestX18/X19; no arbitrary gueststack/SDK/gameboot/exclusive-monitor/performance claim"}
    (out / "verification.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"host_tests": tests, "helper_sha256": sha(library), "android_cross_link_passed": True,
                      "device_execution": record["device_execution"]}, indent=2))


if __name__ == "__main__":
    main()
