#!/usr/bin/env python3
"""Prepare an asset-private, single-leaf CPU3/native comparison; no ADB or guest initialization."""
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

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def run(arguments, log):
    process = subprocess.run([str(x) for x in arguments], capture_output=True, text=True, timeout=60)
    log.write_text(process.stdout + process.stderr)
    require(process.returncode == 0, "Check failed; inspect " + str(log.relative_to(ROOT)))
    return process.stdout


def verify_candidate(path):
    import capstone
    candidate = json.loads(path.read_text())
    prepare_path = Path(candidate["aot"]["prepare_path"])
    prepare = json.loads(prepare_path.read_text())
    semantic = prepare["semantic"]
    require(semantic["title"] == "las" and semantic["cpu_identity"] == 3,
            "Only the measured LAS CPU3 candidate is supported")
    require(prepare["semantic_code_sha256"] == candidate["aot"]["semantic_code_sha256"], "Producer semantic identity changed")
    Nso, _, _ = load_parser()
    originals = {}
    for name, module in semantic["modules"].items():
        original = Path(module["path"])
        require(sha(original) == module["sha256"], "Original module SHA differs from producer")
        require(original.read_bytes()[0x40:0x60].hex().lower() == module["build_id"].lower(), "Original module full Build ID differs")
        originals[name] = {"sha256": module["sha256"], "build_id": module["build_id"]}
    original = Path(candidate["original"]["path"])
    sdk = semantic["modules"]["sdk"]
    require(original.resolve() == Path(sdk["path"]).resolve() and sha(original) == candidate["original"]["sha256"] == sdk["sha256"],
            "Candidate SDK identity changed")
    nso = Nso(original)
    text_offset, text_size = nso.text
    require(hashlib.sha256(nso.image[text_offset:text_offset + text_size]).hexdigest() == candidate["original"]["decompressed_text_sha256"],
            "Original text identity changed")
    symbol = [s for s in nso.syms if s[0] == "fabsf" and s[2] and s[1] & 15 == 2]
    require(len(symbol) == 1 and symbol[0][4] == 8, "Reviewed leaf symbol/size changed")
    offset = symbol[0][3]
    require(text_offset <= offset and offset + 8 <= text_offset + text_size, "Leaf is outside original text")
    code = bytes(nso.image[offset:offset + 8])
    require(code.hex() == candidate["candidate"]["original_bytes_hex"] and
            hashlib.sha256(code).hexdigest() == candidate["candidate"]["code_sha256"], "Reviewed leaf bytes changed")
    require(not any(offset <= relocation[0] < offset + 8 for relocation in nso.relocs), "Relocation writes into leaf")
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    decoded = list(md.disasm(code, 0))
    require([(i.mnemonic, i.op_str.replace(" ", "")) for i in decoded] == [("fabs", "s0,s0"), ("ret", "")],
            "Instruction whitelist rejected candidate")
    actual = sdk["canonical_base"] + offset - semantic["guest_shift"]
    require(actual == candidate["candidate"]["actual_cpu3_address"], "Candidate address changed")
    generated = Path(prepare["generated_directory"])
    table = Path(candidate["aot"]["generated_table_path"])
    unit = Path(candidate["aot"]["definition_unit"])
    for source in (table, unit):
        key = str(source.relative_to(generated))
        require(sha(source) == semantic["generated_sha256"][key], "Generated table/unit no longer matches producer")
    require(sha(table) == candidate["aot"]["generated_table_sha256"] and
            sha(unit) == candidate["aot"]["definition_unit_sha256"], "Reviewed AOT sources changed")
    text = table.read_text()
    addresses = re.search(r"const uint64_t sdk_fn_addrs\[\]\s*=\s*\{(.*?)\};", text, re.S)
    functions = re.search(r"const GuestFn sdk_fn_ptrs\[\]\s*=\s*\{(.*?)\};", text, re.S)
    require(addresses and functions, "Generated table format changed")
    addrs = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)ull", addresses[1])]
    names = re.findall(r"\bf_([0-9a-fA-F]+)\b", functions[1])
    require(len(addrs) == len(names) == sdk["function_count"] and actual in addrs, "AOT table/count missing")
    index = addrs.index(actual)
    require(int(names[index], 16) == actual + semantic["guest_shift"], "AOT table slot/address mismatch")
    cases = candidate["candidate"]["proposed_cases"]
    require(len(cases) == 12, "Expected exactly the twelve reviewed cases")
    for case in cases:
        bits, expected = int(case["input_bits"], 16), int(case["expected_result_bits"], 16)
        magnitude = bits & 0x7fffffff
        require(0 <= bits <= 0xffffffff and expected == magnitude and
                (magnitude == 0 or magnitude == 0x7f800000 or 0x00800000 <= magnitude <= 0x7f7fffff),
                "Unreviewed floating input or expected result")
    return candidate, prepare, code, cases, actual, originals


DEVICE_RUNNER = '''import base64,ctypes,hashlib,json,os,sys
config=json.load(open(sys.argv[1]));runtime,game,helper=sys.argv[2:5]
for path,key in ((runtime,'runtime_sha256'),(game,'game_sha256'),(helper,'helper_sha256')):
    with open(path,'rb') as stream: actual=hashlib.file_digest(stream,'sha256').hexdigest()
    if actual!=config[key]: raise SystemExit('SHA256 mismatch: '+key)
code=bytes.fromhex(config['leaf_hex']);manifest=base64.b64decode(config['expected_manifest_base64']);contract=bytes.fromhex(config['contract_sha256'])
if hashlib.sha256(code).hexdigest()!=config['leaf_sha256'] or hashlib.sha256(manifest).hexdigest()!=config['expected_manifest_sha256']: raise SystemExit('Private probe input mismatch')
if manifest[144:176].hex()!=config['game_sha256']: raise SystemExit('Probe library expectation mismatch')
lib=ctypes.CDLL(helper,mode=ctypes.RTLD_LOCAL);probe=lib.native_las_leaf_probe
probe.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_uint64,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_uint32),ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t];probe.restype=ctypes.c_int
cases=config['cases'];inputs=(ctypes.c_uint32*len(cases))(*(int(x['input_bits'],16) for x in cases));expected=(ctypes.c_uint32*len(cases))(*(int(x['expected_result_bits'],16) for x in cases))
buffers=[ctypes.create_string_buffer(x) for x in (manifest,contract,code)];output=ctypes.create_string_buffer(8192)
status=probe(os.fsencode(runtime),os.fsencode(game),buffers[0],len(manifest),buffers[1],config['actual_cpu3_address'],buffers[2],len(code),inputs,expected,len(cases),output,len(output))
report=json.loads(output.value.decode());report['runtime_sha256']=config['runtime_sha256'];report['game_sha256']=config['game_sha256'];report['helper_sha256']=config['helper_sha256'];report['leaf_sha256']=config['leaf_sha256'];print(json.dumps(report),flush=True)
raise SystemExit(status!=0)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, default=ROOT / "evidence/native-leaf-candidate.json")
    parser.add_argument("--game-sha256", required=True, help="Coordinator-verified completed on-device game library SHA256")
    parser.add_argument("--runtime-sha256", required=True, help="Exact installed runtime SHA256 used by that library")
    parser.add_argument("--ndk", type=Path, default=Path.home() / "android-sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    require(all(re.fullmatch(r"[0-9a-f]{64}", x) for x in (args.game_sha256, args.runtime_sha256)), "Expected full lowercase SHA256 values")
    require(args.candidate.resolve().is_relative_to(ROOT / "evidence") and
            subprocess.run(["git", "check-ignore", "-q", str(args.candidate)], cwd=ROOT).returncode == 0,
            "Candidate must remain in ignored evidence")
    gate_pointer = ROOT / "evidence/native-arm64-device-current.json"
    gate_path = ROOT / json.loads(gate_pointer.read_text())["directory"] / "report.json"
    gate = json.loads(gate_path.read_text())
    require(len(gate["runs"]) >= 2 and all(r["returncode"] == 0 and r["report"]["state"] == "passed" and
            r["report"]["signals"] == 2 and r["report"]["callbacks"] == 256 for r in gate["runs"]), "Hardware bridge gate has not passed twice")
    require(gate["artifact_sha256"] == sha(ROOT / "build/native-arm64-proof/libnative_arm64_proof.so"), "Hardware gate artifact differs from frozen proof")
    candidate, prepare, code, cases, actual, originals = verify_candidate(args.candidate)
    out = ROOT / "build/native-las-leaf-proof"
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / "tests/native_las_leaf_probe.c"
    cc = shutil.which("clang")
    require(cc, "Installed host clang required")
    host = {}
    for kind, flags in (("optimized", ["-O2"]), ("sanitized", ["-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = out / ("whitelist-" + kind)
        run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DLEAF_HOST_TEST", *flags,
             "-I", ROOT / "native", source, "-o", binary], out / (kind + "-build.log"))
        text = run([binary], out / (kind + "-run.log"))
        host[kind] = {"passed": True, "assertions": int(re.search(r"assertions=(\d+)", text)[1])}
    toolchain = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    require("27.2.12479018" in (args.ndk / "source.properties").read_text(), "Use cached NDK r27c")
    exports = out / "exports.map"
    exports.write_text("LEAF_PROBE_1 { global: native_las_leaf_probe; local: *; };\n")
    library = out / "libnative_las_leaf_probe.so"
    run([toolchain / "aarch64-linux-android30-clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
         "-fPIC", "-ffixed-x18", "-fvisibility=hidden", "-shared", "-I", ROOT / "native", source,
         ROOT / "native/module_abi.c", ROOT / "native/runtime/title_manifest.c", "-ldl",
         "-Wl,--no-undefined", "-Wl,--version-script=" + str(exports), "-Wl,-z,max-page-size=16384",
         "-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack", "-o", library], out / "android-build.log")
    surface = run([toolchain / "llvm-nm", "--dynamic", "--defined-only", library], out / "exports.txt")
    require([line.split()[-1].split("@")[0] for line in surface.splitlines()] == ["native_las_leaf_probe"], "Helper export surface mismatch")
    expected = bytearray((ROOT / "build/shared-gamelib/las/liblasgame.so.saotm").read_bytes())
    require(expected[:8] == b"SAOTM001" and expected[80:112].hex() == prepare["semantic"]["contract"] and
            expected[112:144].hex() == prepare["semantic_code_sha256"], "Host sidecar/producer identity mismatch")
    modules = list(prepare["semantic"]["modules"].values())
    require(len(expected) == 176 + 112 * len(modules) and struct.unpack_from("<I", expected, 52)[0] == len(modules),
            "Host sidecar module inventory differs from producer")
    for index, module in enumerate(modules):
        independent = struct.pack("<II32s32sQQQQQ", module["module_id"], 0,
                                  bytes.fromhex(module["build_id"]), bytes.fromhex(module["sha256"]),
                                  module["canonical_base"], module["mapped_size"], module["text_offset"],
                                  module["text_size"], module["function_count"])
        require(expected[176 + index * 112:176 + (index + 1) * 112] == independent,
                "Host sidecar module expectations differ from independently hashed producer metadata")
    # Probe-only independent expectation: actual original module identities and
    # unchanged producer contract/code, with coordinator's measured device ELF.
    expected[144:176] = bytes.fromhex(args.game_sha256)
    config = {"schema": 1, "symbol": "fabsf", "runtime_sha256": args.runtime_sha256, "game_sha256": args.game_sha256,
              "helper_sha256": sha(library), "leaf_hex": code.hex(), "leaf_sha256": hashlib.sha256(code).hexdigest(),
              "actual_cpu3_address": actual, "contract_sha256": prepare["semantic"]["contract"], "cases": cases,
              "expected_manifest_base64": base64.b64encode(expected).decode(), "expected_manifest_sha256": hashlib.sha256(expected).hexdigest()}
    config_path = out / "private-config.json"
    config_path.write_text(json.dumps(config, indent=2) + "\n")
    launcher = out / "run_leaf.py"
    launcher.write_text(DEVICE_RUNNER)
    sources = [Path(__file__).resolve(), source, ROOT / "native/runtime/aot_cpu.h", ROOT / "native/module_abi.h",
               ROOT / "native/module_abi.c", ROOT / "native/runtime/title_manifest.h", ROOT / "native/runtime/title_manifest.c"]
    record = {"schema": 1, "host_tests": host, "android_cross_link_passed": True, "device_execution": "pending-coordinator",
              "hardware_gate_sha256": sha(gate_path), "candidate_sha256": sha(args.candidate), "originals": originals,
              "sources_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sources},
              "artifacts_sha256": {str(p.relative_to(ROOT)): sha(p) for p in (library, config_path, launcher)},
              "device_game_library_sha256_supplied_by_coordinator": args.game_sha256,
              "device_runtime_sha256_supplied_by_coordinator": args.runtime_sha256,
              "scope": "12 fabsf cases only; actual descriptor function, no guest initialization or full backend"}
    (out / "verification.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"host_tests": host, "android_cross_link_passed": True,
                      "helper_sha256": sha(library), "device_execution": record["device_execution"]}, indent=2))


if __name__ == "__main__":
    main()
