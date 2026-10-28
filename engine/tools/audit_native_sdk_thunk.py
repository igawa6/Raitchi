#!/usr/bin/env python3
"""Audit a symbol-resolved original SDK wrapper and its bounded SVC thunk.

Pinned NSO parser plus Capstone evidence, not REA analysis or game execution.
Original code, addresses and generated C excerpts are written only to ignored
local evidence/build paths. No proprietary bytes or addresses are embedded here.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from audit_game_inputs import load_parser

ROOT = Path(__file__).resolve().parents[1]
SYMBOL = "_ZNK2nn2os6detail26ThreadManagerImplByHorizon20GetCurrentCoreNumberEv"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def ignored(path, parent):
    path = path.resolve()
    require(path.is_relative_to(parent), "Private output must remain in its ignored workspace directory")
    require(subprocess.run(["git", "check-ignore", "-q", str(path)], cwd=ROOT).returncode == 0,
            "Private output is not ignored by Git")
    return path


def definitions(generated, names, hashes):
    found = {name: [] for name in names}
    for unit in (generated / "sdk").glob("*.c"):
        text = unit.read_text()
        for name in names:
            match = re.search(r"\b" + name + r"\([^)]*\)\s*\{", text)
            if not match:
                continue
            end, depth = match.end(), 1
            while depth and end < len(text):
                depth += (text[end] == "{") - (text[end] == "}")
                end += 1
            require(depth == 0, "Generated function braces are incomplete")
            digest = sha(unit)
            require(digest == hashes[str(unit.relative_to(generated))], "Generated definition differs from producer")
            found[name].append({"definition_unit": str(unit), "definition_unit_sha256": digest,
                                "definition_excerpt": text[match.start():end]})
    require(all(len(values) == 1 for values in found.values()), "Expected unique wrapper and thunk AOT definitions")
    return {name: values[0] for name, values in found.items()}


def main():
    import capstone
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--previous", type=Path, default=ROOT / "evidence/native-memory-candidate.json")
    parser.add_argument("--prepare", type=Path, default=ROOT / "build/shared-gamelib/las/prepare.json")
    parser.add_argument("--output", type=Path, default=ROOT / "evidence/native-sdk-thunk-candidate.json")
    parser.add_argument("--build", type=Path, default=ROOT / "build/native-sdk-thunk-candidate")
    args = parser.parse_args()
    output = ignored(args.output, ROOT / "evidence")
    build = ignored(args.build, ROOT / "build")
    previous = json.loads(args.previous.read_text())
    prepare = json.loads(args.prepare.read_text())
    semantic = prepare["semantic"]
    require(semantic["title"] == "las" and semantic["cpu_identity"] == 3, "Expected measured LAS CPU3 producer")
    require(hashlib.sha256(json.dumps(semantic, sort_keys=True).encode()).hexdigest() == prepare["semantic_code_sha256"],
            "Producer semantic fingerprint mismatch")
    generation_manifest = Path(prepare["generation_manifest"])
    require(sha(generation_manifest) == semantic["generation_manifest_sha256"], "Generation manifest differs from producer")
    originals = {}
    for name, module in semantic["modules"].items():
        original = Path(module["path"])
        require(sha(original) == module["sha256"] and original.read_bytes()[0x40:0x60].hex() == module["build_id"].lower(),
                "Original module SHA/full Build ID changed")
        originals[name] = {"sha256": module["sha256"], "full_nso_build_id": module["build_id"]}
    sdk = semantic["modules"]["sdk"]
    original = Path(previous["original"]["path"])
    require(original.resolve() == Path(sdk["path"]).resolve() and sha(original) == previous["original"]["sha256"] == sdk["sha256"],
            "Previously audited SDK identity changed")
    Nso, backend, parser_path = load_parser()
    require(sha(parser_path) == semantic["generation"]["source_sha256"]["tools/nso.py"] == previous["parser_sha256"],
            "Pinned parser differs from original audit/producer")
    nso = Nso(original)
    text_start, text_size = nso.text
    text_hash = hashlib.sha256(nso.image[text_start:text_start + text_size]).hexdigest()
    require(text_start == sdk["text_offset"] and text_size == sdk["text_size"] and
            text_hash == previous["original"]["decompressed_text_sha256"], "Original SDK text identity changed")
    symbols = [s for s in nso.syms if s[0] == SYMBOL and s[2] and s[1] & 15 == 2]
    require(len(symbols) == 1 and symbols[0][4] == 4, "Expected one four-byte exported wrapper")
    wrapper_offset = symbols[0][3]
    require(wrapper_offset % 4 == 0 and text_start <= wrapper_offset and wrapper_offset + 4 <= text_start + text_size,
            "Wrapper is unaligned or outside SDK text")
    branch_word = struct.unpack_from("<I", nso.image, wrapper_offset)[0]
    require(branch_word & 0xfc000000 == 0x14000000, "Wrapper is not an unconditional immediate B")
    displacement = branch_word & ((1 << 26) - 1)
    if displacement & (1 << 25):
        displacement -= 1 << 26
    thunk_offset = wrapper_offset + displacement * 4
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    records, bodies = {}, {}
    require(set(r[1] for r in nso.relocs) <= {257, 1025, 1026, 1027}, "Unknown relocation write width")
    for label, offset, size in (("wrapper", wrapper_offset, 4), ("thunk", thunk_offset, 8)):
        require(offset % 4 == 0 and text_start <= offset and offset + size <= text_start + text_size,
                "Candidate is unaligned or outside SDK text")
        overlapping = [list(r) for r in nso.relocs if r[0] < offset + size and r[0] + 8 > offset]
        require(not overlapping, "Relocation write overlaps candidate")
        code = bytes(nso.image[offset:offset + size])
        canonical = sdk["canonical_base"] + offset
        decoded = list(md.disasm(code, canonical))
        require(sum(i.size for i in decoded) == size and all(i.size == 4 for i in decoded), "Incomplete ARM64 decode")
        bodies[label] = code
        records[label] = {"offset": offset, "module_offset": offset, "size": size,
                          "canonical_address": canonical, "actual_cpu3_address": canonical - semantic["guest_shift"],
                          "code_sha256": hashlib.sha256(code).hexdigest(), "original_bytes_hex": code.hex(),
                          "relocation_writes_overlapping": [], "aligned_4": True, "within_original_text": True,
                          "instructions": [{"address": i.address, "mnemonic": i.mnemonic, "operands": i.op_str} for i in decoded]}
        if label == "wrapper":
            require(len(decoded) == 1 and decoded[0].mnemonic == "b" and len(decoded[0].operands) == 1 and
                    decoded[0].operands[0].type == capstone.arm64.ARM64_OP_IMM and
                    decoded[0].operands[0].imm == sdk["canonical_base"] + thunk_offset,
                    "Capstone target disagrees with original branch decoding")
        else:
            require(len(decoded) == 2 and decoded[0].mnemonic == "svc" and len(decoded[0].operands) == 1 and
                    decoded[0].operands[0].type == capstone.arm64.ARM64_OP_IMM and decoded[0].operands[0].imm == 0x10 and
                    decoded[1].mnemonic == "ret" and not decoded[1].op_str,
                    "Tail target is not exactly SVC10 followed by RET LR")
    generated = Path(prepare["generated_directory"])
    table = generated / "sdk/sdk_table.c"
    require(sha(table) == semantic["generated_sha256"][str(table.relative_to(generated))], "AOT table differs from producer")
    table_text = table.read_text()
    addresses = re.search(r"const uint64_t sdk_fn_addrs\[\]\s*=\s*\{(.*?)\};", table_text, re.S)
    pointers = re.search(r"const GuestFn sdk_fn_ptrs\[\]\s*=\s*\{(.*?)\};", table_text, re.S)
    require(addresses and pointers, "AOT table shape changed")
    addrs = [int(s, 16) for s in re.findall(r"0x([0-9a-fA-F]+)ull", addresses[1])]
    names = re.findall(r"\bf_[0-9a-fA-F]+\b", pointers[1])
    require(len(addrs) == len(names) == sdk["function_count"], "AOT table counts differ from producer")
    candidate_names = ["f_" + format(records[label]["canonical_address"], "x") for label in ("wrapper", "thunk")]
    found = definitions(generated, candidate_names, semantic["generated_sha256"])
    aot = {"prepare_path": str(args.prepare.resolve()), "prepare_sha256": sha(args.prepare),
           "semantic_code_sha256": prepare["semantic_code_sha256"], "table_path": str(table), "table_sha256": sha(table),
           "actual_address": records["wrapper"]["actual_cpu3_address"],
           "generation_manifest_sha256": sha(generation_manifest), "source_commit": semantic["generation"]["source_commit"]}
    for label, name in zip(("wrapper", "thunk"), candidate_names):
        actual = records[label]["actual_cpu3_address"]
        require(addrs.count(actual) == 1 and names[addrs.index(actual)] == name, "AOT pointer/address provenance mismatch")
        aot[label] = {"table_index": addrs.index(actual), "function_name": name, "actual_address": actual, **found[name]}
    require(re.search(r"__attribute__\(\(musttail\)\)\s+return\s+" + candidate_names[1] + r"\(c\);",
                      aot["wrapper"]["definition_excerpt"]), "AOT wrapper does not tail-call original thunk")
    require(re.search(r"rt_svc\(c,\s*16,\s*0x" + format(records["thunk"]["actual_cpu3_address"], "x") +
                      r"ull\);\s*return;", aot["thunk"]["definition_excerpt"]), "AOT thunk does not call production SVC10 and return")
    aot["wrapper"]["behavior"] = "FN_ENTER wrapper; musttail return thunk(c); trailing rt_udf calls unreachable after musttail"
    aot["thunk"]["behavior"] = "FN_ENTER thunk; rt_svc(c,16,actual_thunk_pc); return"
    build.mkdir(parents=True, exist_ok=True)
    for label, code in bodies.items():
        destination = ignored(build / (label + ".bin"), ROOT / "build")
        destination.write_bytes(code)
        records[label]["path"] = str(destination)
    bundle = bodies["wrapper"] + bodies["thunk"]
    bundle_path = ignored(build / "bundle.bin", ROOT / "build")
    bundle_path.write_bytes(bundle)
    report = {"schema": 1, "state": "audited-not-device-executed", "verified_utc": datetime.now(timezone.utc).isoformat(),
              "method": "pinned NSO parser and Capstone; not REA evidence", "device_actions": False,
              "original": {"path": str(original), **originals["sdk"], "text_offset": text_start, "text_bytes": text_size,
                           "decompressed_text_sha256": text_hash, "producer_original_hash_matches": True},
              "originals": originals, "symbol": list(symbols[0]), "parser_path": str(parser_path),
              "parser_sha256": sha(parser_path), "lz4_backend": backend, "capstone": capstone.__version__,
              "guest_shift": semantic["guest_shift"], **records,
              "original_branch": {"site": records["wrapper"]["canonical_address"],
                                  "target": records["thunk"]["canonical_address"], "target_module_offset": thunk_offset,
                                  "signed_displacement_bytes": displacement * 4, "independent_decoder_matches_capstone": True},
              "bundle": {"path": str(bundle_path), "sha256": hashlib.sha256(bundle).hexdigest(), "size": len(bundle),
                         "format": "original wrapper4 then original thunk8; wrapper B unmodified, relocation required before copied execution"},
              "aot": aot, "script_sha256": sha(Path(__file__).resolve()),
              "scope": "single exported wrapper plus SVC10/RET tail target; no SDK initialization or arbitrary game execution"}
    output.write_text(json.dumps(report, indent=2) + "\n")
    print("SDK wrapper4 + SVC thunk8 audited with parser/Capstone and verified AOT provenance; private evidence only.")


if __name__ == "__main__":
    main()
