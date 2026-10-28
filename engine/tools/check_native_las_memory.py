#!/usr/bin/env python3
"""Prepare a reviewed original SDK memory/branch differential proof; no ADB or full backend."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from audit_game_inputs import load_parser
from check_native_las_leaf import ROOT, require, run, sha


def candidate_audit():
    import capstone
    previous = json.loads((ROOT / "evidence/native-leaf-candidate.json").read_text())
    prepare_path = ROOT / "build/shared-gamelib/las/prepare.json"
    prepare = json.loads(prepare_path.read_text())
    semantic = prepare["semantic"]
    require(semantic["title"] == "las" and semantic["cpu_identity"] == 3, "Expected measured LAS CPU3 producer")
    Nso, _, parser = load_parser()
    for module in semantic["modules"].values():
        original = Path(module["path"])
        require(sha(original) == module["sha256"] and original.read_bytes()[0x40:0x60].hex() == module["build_id"].lower(),
                "Original module SHA/full Build ID changed")
    path = Path(previous["original"]["path"])
    sdk = semantic["modules"]["sdk"]
    require(path.resolve() == Path(sdk["path"]).resolve() and sha(path) == previous["original"]["sha256"] == sdk["sha256"],
            "Previously verified original SDK changed")
    nso = Nso(path)
    start, length = nso.text
    require(hashlib.sha256(nso.image[start:start + length]).hexdigest() == previous["original"]["decompressed_text_sha256"],
            "Previously verified SDK text changed")
    symbols = [s for s in nso.syms if s[0] == "strlen" and s[2] and s[1] & 15 == 2]
    require(len(symbols) == 1 and 8 <= symbols[0][4] <= 256 and symbols[0][4] % 4 == 0, "No bounded exported strlen")
    symbol = symbols[0]
    offset, size = symbol[3:5]
    require(start <= offset and offset + size <= start + length, "Symbol outside original text")
    require(not any(offset <= relocation[0] < offset + size for relocation in nso.relocs), "Relocation writes inside candidate")
    code = bytes(nso.image[offset:offset + size])
    canonical = sdk["canonical_base"] + offset
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    instructions = list(md.disasm(code, canonical))
    require(sum(i.size for i in instructions) == size and instructions[-1].mnemonic == "ret", "Incomplete decode or missing final return")
    allowed = {"mov", "movk", "tst", "b.eq", "cbz", "cbnz", "ldrb", "ldr", "add", "sub", "bic", "ret"}
    branch_targets = []
    for instruction in instructions:
        require(instruction.mnemonic in allowed, "Unreviewed instruction class")
        registers = re.findall(r"\b(?:[xw]\d+|[xw]zr|sp|[bhsdqv]\d+)\b", instruction.op_str)
        require(all(reg in {"x0", "w0", "x8", "w8", "x9", "w9", "x10", "w10", "x11", "w11", "xzr", "wzr"} for reg in registers),
                "Unreviewed register or stack use")
        if instruction.mnemonic in ("b.eq", "cbz", "cbnz"):
            targets = [op.imm for op in instruction.operands if op.type == capstone.arm64.ARM64_OP_IMM]
            require(len(targets) == 1 and canonical <= targets[0] < canonical + size and (targets[0] - canonical) % 4 == 0,
                    "Branch exits copied function")
            branch_targets.append({"site": instruction.address, "target": targets[0]})
    require(branch_targets and any(i.mnemonic in ("ldr", "ldrb") for i in instructions), "Candidate does not exercise memory/branches")
    generated = Path(prepare["generated_directory"])
    table = generated / "sdk/sdk_table.c"
    require(sha(table) == semantic["generated_sha256"][str(table.relative_to(generated))], "Producer table hash mismatch")
    text = table.read_text()
    addrs_match = re.search(r"const uint64_t sdk_fn_addrs\[\]\s*=\s*\{(.*?)\};", text, re.S)
    names_match = re.search(r"const GuestFn sdk_fn_ptrs\[\]\s*=\s*\{(.*?)\};", text, re.S)
    require(addrs_match and names_match, "Producer table shape changed")
    addrs = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)ull", addrs_match[1])]
    names = re.findall(r"\bf_([0-9a-fA-F]+)\b", names_match[1])
    actual = canonical - semantic["guest_shift"]
    require(len(addrs) == len(names) == sdk["function_count"] and actual in addrs, "CPU3 table entry missing")
    slot = addrs.index(actual)
    require(int(names[slot], 16) == canonical, "CPU3 table pointer/address mismatch")
    definitions = []
    name = "f_" + format(canonical, "x")
    for unit in (generated / "sdk").glob("*.c"):
        text = unit.read_text()
        match = re.search(r"\b" + name + r"\([^)]*\)\s*\{", text)
        if match:
            begin, end, depth = match.end() - 1, match.end(), 1
            while depth:
                depth += (text[end] == "{") - (text[end] == "}")
                end += 1
            definitions.append((unit, text[match.start():end]))
    require(len(definitions) == 1, "Expected exactly one generated function definition")
    unit, body = definitions[0]
    require(sha(unit) == semantic["generated_sha256"][str(unit.relative_to(generated))], "Generated function unit hash changed")
    require(not any(token in body for token in ("rt_dispatch", "rt_fnhook", "rt_hook", "GUEST(c->x")), "Leaf requires extra runtime routing or pointer handling")
    record = {"schema": 1, "state": "audited-not-device-executed", "original": previous["original"],
              "parser_sha256": sha(parser), "capstone": capstone.__version__, "symbol": list(symbol),
              "canonical_address": canonical, "actual_cpu3_address": actual, "original_bytes_hex": code.hex(),
              "code_sha256": hashlib.sha256(code).hexdigest(), "instructions": [
                  {"address": i.address, "mnemonic": i.mnemonic, "operands": i.op_str} for i in instructions],
              "internal_branches": branch_targets, "relocations_in_body": [],
              "abi": "size_t strlen(const char*): native X0 actual pointer/length; CPU3 x[0] actual pointer/length",
              "memory_bounds": "byte alignment prefix; aligned8-byte reads may include padding after NUL within its word; guard-adjacent cases keep full word readable",
              "aot": {"prepare_sha256": sha(prepare_path), "semantic_code_sha256": prepare["semantic_code_sha256"],
                      "table_sha256": sha(table), "table_index": slot, "definition_unit": str(unit),
                      "definition_unit_sha256": sha(unit), "definition_excerpt": body}}
    destination = ROOT / "evidence/native-memory-candidate.json"
    require(subprocess.run(["git", "check-ignore", "-q", str(destination)], cwd=ROOT).returncode == 0, "Private evidence must be ignored")
    destination.write_text(json.dumps(record, indent=2) + "\n")
    return prepare, code, actual, destination


RUNNER = '''import base64,ctypes,hashlib,json,os,sys
config=json.load(open(sys.argv[1]));runtime,game,helper=sys.argv[2:5]
for path,key in ((runtime,'runtime_sha256'),(game,'game_sha256'),(helper,'helper_sha256')):
    with open(path,'rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
    if digest!=config[key]: raise SystemExit('SHA256 mismatch: '+key)
code=bytes.fromhex(config['code_hex']);manifest=base64.b64decode(config['manifest_base64']);contract=bytes.fromhex(config['contract_sha256'])
if hashlib.sha256(code).hexdigest()!=config['code_sha256'] or hashlib.sha256(manifest).hexdigest()!=config['manifest_sha256'] or manifest[144:176].hex()!=config['game_sha256']: raise SystemExit('Private input mismatch')
lib=ctypes.CDLL(helper,mode=ctypes.RTLD_LOCAL);probe=lib.native_las_memory_probe
probe.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_uint64,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t];probe.restype=ctypes.c_int
buffers=[ctypes.create_string_buffer(x) for x in (manifest,contract,code)];output=ctypes.create_string_buffer(4096)
status=probe(os.fsencode(runtime),os.fsencode(game),buffers[0],len(manifest),buffers[1],config['actual_cpu3_address'],buffers[2],len(code),output,len(output))
report=json.loads(output.value.decode());report.update({k:config[k] for k in ('runtime_sha256','game_sha256','helper_sha256','code_sha256')});print(json.dumps(report),flush=True)
raise SystemExit(status!=0)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-sha256", required=True)
    parser.add_argument("--game-sha256", required=True)
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    require(all(re.fullmatch(r"[0-9a-f]{64}", s) for s in (args.runtime_sha256, args.game_sha256)), "Full measured hashes required")
    bridge = ROOT / json.loads((ROOT / "evidence/native-arm64-device-current.json").read_text())["directory"] / "report.json"
    bridge_record = json.loads(bridge.read_text())
    require(len(bridge_record["runs"]) >= 2 and all(r["returncode"] == 0 and r["report"]["state"] == "passed" for r in bridge_record["runs"]), "Synthetic hardware gate incomplete")
    require(bridge_record["artifact_sha256"] == sha(ROOT / "build/native-arm64-proof/libnative_arm64_proof.so"), "Synthetic gate artifact changed")
    leaf_dir = ROOT / json.loads((ROOT / "evidence/native-las-leaf-current.json").read_text())["directory"]
    leaf = json.loads((leaf_dir / "stdout.json").read_text())
    require(json.loads((leaf_dir / "status.json").read_text())["returncode"] == 0 and leaf["state"] == "passed" and leaf["cases"] == 12 and leaf["failures"] == 0, "Actual scalar leaf gate incomplete")
    require(len(leaf["results"]) == 12 and all(row["passed"] for row in leaf["results"]) and
            leaf["helper_sha256"] == sha(ROOT / "build/native-las-leaf-proof/libnative_las_leaf_probe.so"), "Scalar gate rows/artifact changed")
    require(args.game_sha256 == leaf["game_sha256"], "Memory proof must use the already compared on-device CPU3 library")
    prepare, code, address, candidate = candidate_audit()
    out = ROOT / "build/native-las-memory-proof"; out.mkdir(parents=True, exist_ok=True)
    raw = out / "candidate.bin"; raw.write_bytes(code)
    source = ROOT / "tests/native_las_memory_probe.c"
    cc = shutil.which("clang"); require(cc, "Installed host clang required")
    host = {}
    for kind, flags in (("optimized", ["-O2"]), ("sanitized", ["-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = out / ("policy-" + kind)
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DMEMORY_HOST_TEST", *flags,
             "-I", ROOT / "native", source, "-o", binary], out / (kind + "-build.log"))
        result = run([binary, raw], out / (kind + "-run.log"))
        host[kind] = {"passed": True, "assertions": int(re.search(r"assertions=(\d+)", result)[1])}
    require("27.2.12479018" in (args.ndk / "source.properties").read_text(), "Use cached NDK r27c")
    tools = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    exports = out / "exports.map"; exports.write_text("MEMORY_PROBE_1 { global: native_las_memory_probe; local: *; };\n")
    library = out / "libnative_las_memory_probe.so"
    run([tools / "aarch64-linux-android30-clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fPIC",
         "-ffixed-x18", "-fvisibility=hidden", "-shared", "-I", ROOT / "native", source,
         ROOT / "native/module_abi.c", ROOT / "native/runtime/title_manifest.c", "-ldl", "-Wl,--no-undefined",
         "-Wl,--version-script=" + str(exports), "-Wl,-z,max-page-size=16384", "-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack",
         "-o", library], out / "android-build.log")
    surface = run([tools / "llvm-nm", "--dynamic", "--defined-only", library], out / "exports.txt")
    require([line.split()[-1].split("@")[0] for line in surface.splitlines()] == ["native_las_memory_probe"], "Unexpected helper export")
    manifest = bytearray((ROOT / "build/shared-gamelib/las/liblasgame.so.saotm").read_bytes())
    semantic = prepare["semantic"]; modules = list(semantic["modules"].values())
    require(len(manifest) == 176 + 112 * len(modules) and manifest[:8] == b"SAOTM001" and manifest[80:112].hex() == semantic["contract"] and
            manifest[112:144].hex() == prepare["semantic_code_sha256"], "Sidecar/producer mismatch")
    for index, module in enumerate(modules):
        expected = struct.pack("<II32s32sQQQQQ", module["module_id"], 0, bytes.fromhex(module["build_id"]), bytes.fromhex(module["sha256"]),
                               module["canonical_base"], module["mapped_size"], module["text_offset"], module["text_size"], module["function_count"])
        require(manifest[176 + index * 112:176 + (index + 1) * 112] == expected, "Sidecar independent module metadata mismatch")
    manifest[144:176] = bytes.fromhex(args.game_sha256)
    config = {"schema": 1, "runtime_sha256": args.runtime_sha256, "game_sha256": args.game_sha256, "helper_sha256": sha(library),
              "code_hex": code.hex(), "code_sha256": hashlib.sha256(code).hexdigest(), "actual_cpu3_address": address,
              "manifest_base64": base64.b64encode(manifest).decode(), "manifest_sha256": hashlib.sha256(manifest).hexdigest(), "contract_sha256": semantic["contract"]}
    config_path = out / "private-config.json"; config_path.write_text(json.dumps(config, indent=2) + "\n")
    runner = out / "run_memory.py"; runner.write_text(RUNNER)
    sources = [Path(__file__).resolve(), ROOT / "tools/check_native_las_leaf.py", source, ROOT / "native/runtime/aot_cpu.h",
               ROOT / "native/module_abi.h", ROOT / "native/module_abi.c", ROOT / "native/runtime/title_manifest.h", ROOT / "native/runtime/title_manifest.c"]
    record = {"schema": 1, "host_tests": host, "android_cross_link_passed": True, "device_execution": "pending-coordinator",
              "hardware_bridge_gate_sha256": sha(bridge), "actual_leaf_gate_sha256": sha(leaf_dir / "stdout.json"), "candidate_sha256": sha(candidate),
              "sources_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sources},
              "artifacts_sha256": {str(p.relative_to(ROOT)): sha(p) for p in (library, config_path, runner)},
              "scope": "180 original strlen/actual CPU3 input comparisons; partial low guest mapping; no game startup/backend/performance claim"}
    (out / "verification.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"candidate": "strlen", "instructions": len(code) // 4, "host_tests": host,
                      "helper_sha256": sha(library), "device_execution": record["device_execution"]}, indent=2))


if __name__ == "__main__":
    main()
