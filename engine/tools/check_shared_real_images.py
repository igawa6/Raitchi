#!/usr/bin/env python3
"""Private Linux host validation of six original NSOs through the production loader.

No AOT code is compiled/loaded/called, and no Android/device/GPU work is performed.
Exact generated address metadata and strictly enumerated HLE candidates use fatal
never-call host pointers; passing is structural loading evidence, not HLE semantics
or proof that the games run. Detailed private paths/hashes stay in ignored build/.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

from build_shared_gamelib import POLICIES, fingerprint, wire_manifest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    "native/module_abi.c", "native/runtime/nso_image.c",
    "native/runtime/title_manifest.c", "native/runtime/session_layout.c",
    "native/adapters/title_adapter.c", "native/adapters/las/profile.c",
    "native/adapters/letsgo/profile.c", "native/adapters/echoes/profile.c", "native/adapters/wonder/profile.c", "native/runtime/aot_dispatch.c", "native/runtime/native_manifest.c",
)
INPUTS = (*SOURCES, "ports/shared-android/runtime/loader.c",
          "ports/shared-android/runtime/shared_hash.cpp",
          "ports/shared-android/runtime/rt.h", "ports/shared-android/runtime/cpu.h",
          "ports/shared-android/runtime/common_session.h", "native/module_abi.h",
          "native/runtime/nso_image.h", "native/runtime/aot_dispatch.h",
          "native/runtime/aot_cpu.h", "native/runtime/title_manifest.h",
          "native/runtime/session_layout.h", "native/adapters/title_adapter.h",
          "sources/las-android/runtime/platform.h", "sources/las-android/runtime/cache_sha256.h",
          "tools/check_shared_real_images.py", "tools/build_shared_gamelib.py")

HARNESS = r'''
#define _GNU_SOURCE
#include "ports/shared-android/runtime/loader.c"
#include "runtime/aot_dispatch.h"
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
static TitleManifest manifest;
static ModuleAbiDescriptor descriptor;
static ModuleAbiModule modules[4];
static unsigned commits, dummy_calls, hle_hits, registrations;
static char *names[8192];
static unsigned name_count;
static unsigned registered[8192];
static void dummy(Cpu *cpu) { (void)cpu; dummy_calls++; _exit(90); }
const NativeManifest *shared_native_manifest_current(void) { return NULL; }
const TitleManifest *shared_manifest_current(void) { return &manifest; }
const ModuleAbiDescriptor *shared_gamelib_descriptor(void) { return &descriptor; }
void rt_fatal_msg(const char *fmt, ...) {
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
}
void rt_fatal_trace(void) { fprintf(stderr,"rejected: commits=%u dummy_calls=%u\n",commits,dummy_calls); _exit(42); }
static void require(int value, const char *why) { if (!value) FATAL("harness: %s\n",why); }
void plat_commit(uint64_t address, uint64_t length) {
    require(commits < manifest.profile.module_count, "unexpected extra mapping");
    const ModuleAbiModuleSpec *spec = &manifest.modules[commits];
    require(address == spec->canonical_base - manifest.profile.identity.guest_shift && length == spec->mapped_size,
            "commit differs from independent manifest range");
    void *mapped = mmap((void *)(uintptr_t)address, length, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
    if (mapped == MAP_FAILED) FATAL("safe mmap failed: %s\n",strerror(errno));
    if ((uintptr_t)mapped != address) { munmap(mapped,length); FATAL("kernel did not honor exact safe mapping\n"); }
    fprintf(stderr,"mapped %u %llx %llx\n",commits,(unsigned long long)address,(unsigned long long)length);
    commits++;
}
static int find_name(const char *name) {
    unsigned lo=0, hi=name_count;
    while(lo<hi) { unsigned mid=lo+(hi-lo)/2; if(strcmp(names[mid],name)<0) lo=mid+1; else hi=mid; }
    return lo<name_count && !strcmp(names[lo],name) ? (int)lo : -1;
}
HostFn hle_lookup_impl(const char *name) { if(find_name(name)<0) return NULL; hle_hits++; return dummy; }
uint64_t hle_register(const char *name, HostFn fn) {
    int index=find_name(name); require(index>=0 && fn==dummy,"unlisted HLE registration");
    uint64_t address=HLE_BASE+16*(uint64_t)index;
    require((uint64_t)index*16+16<=HLE_SIZE,"HLE metadata capacity");
    if(!registered[index]) {
        registered[index]=1; registrations++;
        fprintf(stderr,"hle-address-standin %s\n",name);
    }
    /* Address-only stand-in: no platform HLE implementation is executed. */
    return address;
}
static uint8_t *read_all(const char *path,size_t *length) {
    FILE *file=fopen(path,"rb"); require(file!=NULL,"open metadata");
    require(!fseek(file,0,SEEK_END),"seek metadata"); long n=ftell(file);
    require(n>=0 && !fseek(file,0,SEEK_SET),"metadata length");
    uint8_t *bytes=malloc((size_t)n+1); require(bytes!=NULL,"metadata allocation");
    require(fread(bytes,1,(size_t)n,file)==(size_t)n && !fclose(file),"read metadata");
    bytes[n]=0; *length=(size_t)n; return bytes;
}
int main(int argc,char **argv) {
    require(argc==5,"arguments: sidecar address-metadata hle-list absolute-exefs");
    require(argv[4][0]=='/',"absolute original directory required");
    size_t length; uint8_t *wire=read_all(argv[1],&length); ModuleAbiFailure failure;
    require(length>=TITLE_MANIFEST_HEADER_SIZE,"manifest header");
    require(title_manifest_parse(wire,length,wire+80,&manifest,&failure)==TITLE_MANIFEST_OK,"manifest parse");
    free(wire);
    size_t address_size; uint8_t *addresses=read_all(argv[2],&address_size); size_t offset=0;
    for(unsigned i=0;i<manifest.profile.module_count;i++) {
        uint64_t count=manifest.modules[i].function_count;
        require(count<=SIZE_MAX/8 && offset<=address_size && count*8<=address_size-offset,"address metadata bounds");
        ModuleAbiGuestFn *functions=malloc((size_t)count*sizeof(*functions)); require(functions!=NULL,"pointer metadata allocation");
        for(uint64_t j=0;j<count;j++) functions[j]=dummy;
        modules[i]=(ModuleAbiModule){.struct_size=sizeof(ModuleAbiModule),.image=manifest.modules[i],
          .address_count=count,.addresses=(const uint64_t *)(addresses+offset),.functions=functions};
        offset+=(size_t)count*8;
    }
    require(offset==address_size,"trailing address metadata");
    descriptor=(ModuleAbiDescriptor){.struct_size=sizeof descriptor,.module_count=manifest.profile.module_count,
       .identity=manifest.profile.identity,.modules=modules};
    SessionLayoutSpec layout; TitleAdapterFailure adapter_failure; SessionLayoutFailure layout_failure;
    require(title_adapter_build_layout((enum TitleId)manifest.title_id,&manifest.profile,manifest.capabilities,
                                      (uint32_t)sysconf(_SC_PAGESIZE),&layout,&adapter_failure)==TITLE_ADAPTER_OK,"title layout");
    require(title_adapter_select((enum TitleId)manifest.title_id)==TITLE_ADAPTER_OK,"immutable title select");
    require(session_layout_activate(&layout,&layout_failure)==SESSION_LAYOUT_OK,"immutable layout activation");
    require(aot_dispatch_bind(&descriptor,&manifest.profile,manifest.capabilities,title_adapter_selected()->capabilities,&failure)==AOT_OK,
            "production dispatch metadata bind");
    FILE *list=fopen(argv[3],"r"); require(list!=NULL,"HLE list"); char *line=NULL; size_t capacity=0; ssize_t size;
    while((size=getline(&line,&capacity,list))>=0) {
        require(name_count<8192 && size>1 && line[size-1]=='\n',"HLE list record"); line[size-1]=0;
        require(!name_count || strcmp(names[name_count-1],line)<0,"HLE list strictly sorted");
        names[name_count++]=strdup(line); require(names[name_count-1]!=NULL,"HLE name allocation");
    }
    free(line); require(!ferror(list) && !fclose(list),"HLE list read");
    loader_load(argv[4]);
    require(g_nmodules==(int)manifest.profile.module_count && commits==manifest.profile.module_count,"published module count");
    for(unsigned i=0;i<commits;i++) {
        const ModuleAbiModuleSpec *spec=&manifest.modules[i]; Module *m=&g_modules[i];
        require(m->base==spec->canonical_base-manifest.profile.identity.guest_shift && m->size==spec->mapped_size,
                "published base/size");
        require(m->text_off==spec->text_offset && m->text_size==spec->text_size,"published text");
        require(!strcmp(m->name,title_adapter_selected()->modules[i].name),"published order");
        if(munmap((void *)(uintptr_t)m->base,m->size)) FATAL("unmap failed\n");
    }
    require(!dummy_calls,"no guest/HLE function calls");
    printf("{\"module_count\":%u,\"exact_maps\":%u,\"dummy_calls\":%u,\"hle_lookups_matched\":%u,\"hle_address_standins\":%u,\"maps\":[",
        commits,commits,dummy_calls,hle_hits,registrations);
    for(unsigned i=0;i<commits;i++)
        printf("%s{\"module\":\"%s\",\"address\":%llu,\"size\":%llu}",i?",":"",g_modules[i].name,
               (unsigned long long)g_modules[i].base,(unsigned long long)g_modules[i].size);
    printf("]}\n");
    return 0;
}
'''


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def checked(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, timeout=90, **kwargs)


def profile(title, out, provenance):
    policy = POLICIES[title]
    prepare_path = ROOT / f"build/shared-gamelib/{title}/prepare.json"
    prepare = json.loads(prepare_path.read_text())
    semantic = prepare["semantic"]
    if prepare["semantic_code_sha256"] != fingerprint(semantic):
        raise RuntimeError("Producer semantic fingerprint changed")
    contract = hashlib.sha256(b"shared-aot-v1\0" + (ROOT / "native/runtime/aot_cpu.h").read_bytes()
                              + (ROOT / "native/module_abi.h").read_bytes()).hexdigest()
    if (semantic["contract"] != contract or semantic["title_id"] != policy["id"]
            or semantic["guest_shift"] != policy["shift"] or semantic["capabilities"] != policy["caps"]
            or semantic["cpu_identity"] != 3 or semantic["cpu_size"] != 66560):
        raise RuntimeError("Current common ABI differs from producer metadata")
    audit_path = ROOT / "evidence/game-input-audit.json"
    audit = json.loads(audit_path.read_text())["titles"][policy["input"]]
    import_path = ROOT / f'evidence/{policy["input"]}-inputs.json'
    imported = json.loads(import_path.read_text())["modules"]
    generation_path = Path(prepare["generation_manifest"])
    if sha(generation_path) != semantic["generation_manifest_sha256"]:
        raise RuntimeError("Generation manifest changed")
    generation = json.loads(generation_path.read_text())
    if generation["generated_sha256"] != semantic["generated_sha256"]:
        raise RuntimeError("Generation hash inventory mismatch")
    if list(semantic["modules"]) != [name for name, _, _ in policy["modules"]] or audit["selected_modules"] != list(semantic["modules"]):
        raise RuntimeError("Independent selected module order differs")
    metadata = {}
    tables = {}
    binary = bytearray()
    for name, module_id, base in policy["modules"]:
        m = semantic["modules"][name]
        a = audit["modules"][name]
        original = Path(m["path"]).resolve()
        if original != (ROOT / a["path"]).resolve():
            raise RuntimeError("Independent original path differs")
        raw = original.read_bytes()
        if len(raw) < 256 or raw[:4] != b"NSO0":
            raise RuntimeError("Original is not an NSO")
        text = struct.unpack_from("<III", raw, 0x10)
        data = struct.unpack_from("<III", raw, 0x30)
        bss = struct.unpack_from("<I", raw, 0x3c)[0]
        mapped = (data[1] + data[2] + bss + 0x3fff) & ~0x3fff
        digest = hashlib.sha256(raw).hexdigest()
        build_id = raw[0x40:0x60].hex()
        if (digest != m["sha256"] or digest != a["sha256"] or digest != imported[name]["sha256"]
                or len(raw) != a["file_bytes"] or build_id != m["build_id"]
                or build_id != a["build_id_32_bytes"].lower()
                or not build_id.startswith(imported[name]["build_id"].lower())
                or m["module_id"] != module_id or m["canonical_base"] != base or a["canonical_base"] != base
                or mapped != m["mapped_size"] or mapped != a["runtime_image_bytes_16k_aligned"]
                or text[1] != m["text_offset"] or text[1] != a["segments"][0]["memory_offset"]
                or text[2] != m["text_size"] or text[2] != a["segments"][0]["uncompressed_bytes"]):
            raise RuntimeError("Original full hash/build/layout differs from independent audits")
        table = Path(prepare["generated_directory"]) / name / f"{name}_table.c"
        relative = f"{name}/{name}_table.c"
        if sha(table) != semantic["generated_sha256"][relative]:
            raise RuntimeError("Generated address table changed")
        source = table.read_text()
        match = re.search(r"const uint64_t " + name + r"_fn_addrs\[\]\s*=\s*\{(.*?)\};", source, re.S)
        ptrs = re.search(r"const GuestFn " + name + r"_fn_ptrs\[\]\s*=\s*\{(.*?)\};", source, re.S)
        count = re.search(r"const uint64_t " + name + r"_fn_count\s*=\s*(\d+)\s*;", source)
        if not match or not ptrs or not count:
            raise RuntimeError("Missing exact generated address metadata")
        addresses = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)ull", match.group(1))]
        functions = [int(x, 16) for x in re.findall(r"\bf_([0-9a-fA-F]+)\b", ptrs.group(1))]
        n = int(count.group(1))
        lo = base - policy["shift"] + text[1]
        if (n != m["function_count"] or len(addresses) != n or len(functions) != n or not n
                or any(x % 4 or not lo <= x < lo + text[2] for x in addresses)
                or any(x >= y for x, y in zip(addresses, addresses[1:]))
                or any(x + policy["shift"] != y for x, y in zip(addresses, functions))):
            raise RuntimeError("Generated addresses/count/order/text correspondence failed")
        if title == "las" and generation["counts"][name] != n:
            raise RuntimeError("Fresh generation count differs")
        binary += struct.pack(f"<{n}Q", *addresses)
        metadata[name] = m
        tables[name] = {"path": str(table), "sha256": sha(table), "count": n}
    # No game library is loaded here. This nonzero identity labels that deliberate
    # stand-in, satisfying the wire format without claiming a real AOT artifact.
    standin = hashlib.sha256(b"host-metadata-only-no-AOT-library-loaded").hexdigest()
    sidecar = wire_manifest(metadata, policy, contract, prepare["semantic_code_sha256"], standin)
    (out / "manifest.bin").write_bytes(sidecar)
    (out / "addresses.bin").write_bytes(binary)
    provenance.update({"prepare_sha256": sha(prepare_path), "generation_manifest_sha256": sha(generation_path),
                       "audit_sha256": sha(audit_path), "import_audit_sha256": sha(import_path),
                       "originals": metadata, "address_tables": tables,
                       "wire_sha256": sha(out / "manifest.bin"), "address_metadata_sha256": sha(out / "addresses.bin"),
                       "wire_library_identity": "labeled metadata-only stand-in; no AOT library loaded"})
    return Path(next(iter(metadata.values()))["path"]).parent


def hle_inventory(out):
    # Inspect the exact source tree staged by the common builder, which contains
    # the overlay replacement files as well as the retained pinned LAS files.
    runtime = ROOT / "work/shared-android/runtime"
    if not runtime.is_dir():
        raise RuntimeError("Common runtime staging missing")
    origins = {}
    source_hashes = {}
    for path in sorted(runtime.rglob("*")):
        if path.suffix not in (".c", ".h", ".cpp", ".mm"):
            continue
        text = path.read_text()
        # A definition must have a literal name, callback identifier and body.
        # Keep string tokens while removing comments to avoid counting examples.
        text = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                      lambda match: "" if match.group(0).startswith("/") else match.group(0), text, flags=re.S)
        found = re.findall(r'\bHLE_FUNC\s*\(\s*"([^"\n]+)"\s*,\s*[A-Za-z_]\w*\s*\)\s*\{', text)
        if found:
            source_hashes[str(path.relative_to(ROOT))] = sha(path)
            for name in found:
                origins.setdefault(name, []).append(str(path.relative_to(ROOT)))
    audit = json.loads((ROOT / "evidence/game-input-audit.json").read_text())
    (out / "hle-staged-literals.txt").write_text("".join(name + "\n" for name in sorted(origins)))
    for registry, values in audit["registries"].items():
        for category in ("literal", "nvn_macro"):
            for name in values[category]:
                origins.setdefault(name, []).append(f"independent-audit:{registry}:{category}")
    names = sorted(origins)
    if not names or len(names) >= 4096 or any("\n" in x for x in names):
        raise RuntimeError("Invalid strict HLE name inventory")
    path = out / "hle-names.txt"
    path.write_text("".join(x + "\n" for x in names))
    (out / "hle-provenance.json").write_text(json.dumps({"names": origins, "staged_source_sha256": source_hashes}, indent=2) + "\n")
    return {"name_count": len(names), "name_list_sha256": sha(path), "provenance_sha256": sha(out / "hle-provenance.json"),
            "staged_source_sha256": source_hashes,
            "policy": "literal HLE_FUNC names in staged common sources or independent audit literal/NVN candidates; never accept arbitrary imports"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--clangxx", default=shutil.which("clang++"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/shared-real-images")
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / "build") or not args.clang or not args.clangxx:
        parser.error("Host Clang/C++ and ignored workspace build output required")
    if sys.platform != "linux":
        parser.error("Linux MAP_FIXED_NOREPLACE host required")
    out.mkdir(parents=True, exist_ok=True)
    before = {name: sha(ROOT / name) for name in INPUTS}
    report = {"schema": 1, "verified_utc": datetime.now(timezone.utc).isoformat(),
              "production_loader": True, "real_original_nsos": True, "sha_provider": "actual shared_hash.cpp CacheSha256",
              "aot_code_compiled_or_loaded": False, "guest_functions_called": False, "device_actions": False,
              "gpu_execution": False, "hle_semantics_proven": False, "source_sha256": before,
              "descriptor_functions": "fatal never-call dummy pointers; exact independently hashed generated address metadata",
              "mapping_policy": "exact module ranges only, RW non-executable MAP_FIXED_NOREPLACE, never overwrite foreign mappings",
              "titles": {}}
    report["hle_inventory"] = hle_inventory(out)
    harness = out / "real_images.c"
    harness.write_text(HARNESS)
    flags = ["-O2", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-format", "-I", ROOT,
             "-I", ROOT / "ports/shared-android/runtime", "-I", ROOT / "sources/las-android/runtime", "-I", ROOT / "native"]
    objects = []
    for index, source in enumerate([harness, *[ROOT / name for name in SOURCES]]):
        obj = out / f"unit-{index}.o"
        checked([args.clang, "-std=c11", *flags, "-c", source, "-o", obj], capture_output=True, text=True)
        objects.append(obj)
    obj = out / "actual-sha.o"
    checked([args.clangxx, "-std=c++17", *flags, "-c", ROOT / "ports/shared-android/runtime/shared_hash.cpp", "-o", obj], capture_output=True, text=True)
    runner = out / "real_images"
    checked([args.clangxx, *objects, obj, "-pthread", "-o", runner], capture_output=True, text=True)
    report["runner_sha256"] = sha(runner)
    report["harness_sha256"] = sha(harness)
    failed = False
    source_import_gap = False
    for title in ("las", "eevee"):
        target = out / title
        target.mkdir(exist_ok=True)
        provenance = {}
        exefs = profile(title, target, provenance)
        command = [runner, target / "manifest.bin", target / "addresses.bin", out / "hle-names.txt", exefs]
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=90)
        (target / "stdout.txt").write_text(result.stdout)
        (target / "stderr.txt").write_text(result.stderr)
        entry = {"command": [str(x) for x in command], "exit": result.returncode, "fresh_process": True,
                 "provenance": provenance, "passed": result.returncode == 0}
        if result.returncode == 0:
            entry["result"] = json.loads(result.stdout)
            entry["hle_address_standin_names"] = re.findall(r"^hle-address-standin (.+)$", result.stderr, re.M)
            if len(entry["hle_address_standin_names"]) != entry["result"]["hle_address_standins"]:
                raise RuntimeError("HLE provenance count differs from actual registration log")
        else:
            failed = True
            entry["bounded_gap"] = "See private stderr; do not treat rejected/missing imports as provided HLE semantics"
        report["titles"][title] = entry
        source_command = [runner, target / "manifest.bin", target / "addresses.bin", out / "hle-staged-literals.txt", exefs]
        source_result = subprocess.run([str(x) for x in source_command], capture_output=True, text=True, timeout=90)
        (target / "staged-literals-stdout.txt").write_text(source_result.stdout)
        (target / "staged-literals-stderr.txt").write_text(source_result.stderr)
        entry["staged_literal_import_coverage"] = {
            "command": [str(x) for x in source_command], "fresh_process": True,
            "exit": source_result.returncode, "passed": source_result.returncode == 0,
            "scope": "literal definitions only; macro-generated providers require separate source audit",
            "unresolved_strong_import": re.findall(r"unresolved strong import (.+)$", source_result.stderr, re.M),
        }
        if source_result.returncode == 0:
            coverage = entry["staged_literal_import_coverage"]
            coverage["result"] = json.loads(source_result.stdout)
            coverage["hle_address_standin_names"] = re.findall(r"^hle-address-standin (.+)$", source_result.stderr, re.M)
            if len(coverage["hle_address_standin_names"]) != coverage["result"]["hle_address_standins"]:
                raise RuntimeError("Strict HLE provenance count differs")
            coverage["audit_only_providers"] = 0
        else:
            source_import_gap = True
            failed = True
        print(f"{title}: {'pass' if result.returncode == 0 else 'rejected'} (fresh process, no guest calls)", flush=True)
        print(f"{title}: staged literal import coverage {'pass' if source_result.returncode == 0 else 'gap; see private report'}", flush=True)
    if before != {name: sha(ROOT / name) for name in INPUTS}:
        raise RuntimeError("Production sources changed during check; rerun against stable snapshot")
    if report["hle_inventory"]["staged_source_sha256"] != {
            name: sha(ROOT / name) for name in report["hle_inventory"]["staged_source_sha256"]}:
        raise RuntimeError("Staged HLE definitions changed during verification")
    report["passed"] = not failed
    report["staged_literal_import_coverage_passed"] = not source_import_gap
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Private report written under ignored build output. Structural loading only; HLE/GPU/gameplay untested.")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
