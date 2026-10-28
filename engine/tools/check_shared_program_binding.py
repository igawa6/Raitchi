#!/usr/bin/env python3
"""Asset-free host checks extracted from the actual shared NVN program binder.

Uses production NvnProgram/NvnShaderStage/CmdRec definitions, BindProgram recorder,
and stage composition helper verbatim. Minimal Cpu/snap/record callbacks isolate
recording and state changes; no shader/GPU/guest/device execution is established.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ("ports/shared-android/runtime/nvn/cmdbuf.c",
          "ports/shared-android/runtime/nvn/vulkan/exec.c",
          "sources/las-android/runtime/nvn/nvn.h",
          "tools/check_shared_program_binding.py")


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def function(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    # Count braces in C tokens; comments and quoted strings cannot end a body.
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]',
                         text[opening:], re.S)
    depth = 0
    for token in tokens:
        value = token.group(0)
        if value == "{":
            depth += 1
        elif value == "}":
            depth -= 1
            if not depth:
                return text[start:opening + token.end()]
    raise RuntimeError("Unterminated production function")


PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef struct { uint64_t x[8]; } Cpu;
typedef struct { uint64_t x[8]; float f[4]; } Snap;
#define NVN(name) static void name(Cpu *c)
static struct { NvnProgram *program; } st;
static CmdRec recorded;
static unsigned records, checks;
static Snap snap(Cpu *c) { Snap s={0}; memcpy(s.x,c->x,sizeof s.x); return s; }
static CmdRec *record(Cpu *c,uint32_t op,uint32_t extra,Snap *s) {
    (void)c; assert(extra==0); memset(&recorded,0,sizeof recorded);
    recorded.op=op; memcpy(recorded.x,s->x+1,sizeof recorded.x); records++; return &recorded;
}
#define CHECK(value) do { checks++; if(!(value)) { fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value); return 1; } } while(0)
'''

TESTS = r'''
static NvnProgram make_program(unsigned mask,uint64_t tag) {
    NvnProgram p={0}; p.stage_mask=mask; p.backend=(void *)(uintptr_t)(tag+0x8000);
    for(unsigned i=0;i<6;i++) if(mask&(1u<<i)) {
        p.stages[i].stage=i; p.stages[i].code_addr=tag+0x100*i;
        p.stages[i].control=(const uint8_t *)(uintptr_t)(tag+0x2000+0x100*i);
        p.stages[i].const_offset=0x80+i; p.stages[i].const_size=0x40+i;
        p.stages[i].backend=(void *)(uintptr_t)(tag+0x4000+0x100*i);
    }
    return p;
}
static void reset(void) { st.program=NULL; memset(&g_comb_prog,0,sizeof g_comb_prog); }
static void recorded_bind(NvnProgram *program,uint32_t mask) {
    /* A guest NVNprogram contains the host pointer in its first word. The real
       HOST macro dereferences this word; null program must never reach HOST. */
    uint64_t guest=(uintptr_t)program; Cpu c={0};
    c.x[1]=program?(uintptr_t)&guest:0; c.x[2]=mask;
    nvnCommandBufferBindProgram(&c);
    bind_program_stages((NvnProgram *)(uintptr_t)recorded.x[0],(uint32_t)recorded.x[1]);
}
int main(void) {
    NvnProgram vf=make_program(3,0x10000),vs=make_program(1,0x20000),fs=make_program(2,0x30000);
    NvnProgram fs2=make_program(2,0x40000),cs=make_program(32,0x50000);
    reset();
    recorded_bind(NULL,1);
    CHECK(records==1 && recorded.op==CMD_BIND_PROGRAM && recorded.x[0]==0 && recorded.x[1]==1);
    CHECK(st.program==NULL);
    recorded_bind(NULL,0); CHECK(st.program==NULL && recorded.x[1]==0);

    /* Existing LAS path: one complete program stays the original pointer. */
    recorded_bind(&vf,3); CHECK(st.program==&vf && recorded.x[0]==(uintptr_t)&vf);
    CHECK(st.program->backend==vf.backend);
    recorded_bind(&vf,0); CHECK(st.program==&vf); /* legacy zero mask */

    /* Eevee-style separate binds retain the other stage and all its metadata. */
    reset(); recorded_bind(&vs,1); CHECK(st.program==&vs);
    recorded_bind(&fs,2); CHECK(st.program==&g_comb_prog && st.program->stage_mask==3);
    CHECK(!memcmp(&st.program->stages[0],&vs.stages[0],sizeof(NvnShaderStage)));
    CHECK(!memcmp(&st.program->stages[1],&fs.stages[1],sizeof(NvnShaderStage)));
    CHECK(st.program->backend==NULL);
    recorded_bind(&cs,32); CHECK(st.program==&g_comb_prog && st.program->stage_mask==35);
    CHECK(st.program->stages[0].code_addr==vs.stages[0].code_addr);
    CHECK(st.program->stages[1].code_addr==fs.stages[1].code_addr);
    CHECK(st.program->stages[5].code_addr==cs.stages[5].code_addr);
    recorded_bind(&fs2,2); CHECK(st.program->stage_mask==35);
    CHECK(st.program->stages[0].code_addr==vs.stages[0].code_addr);
    CHECK(st.program->stages[1].code_addr==fs2.stages[1].code_addr);
    CHECK(st.program->stages[5].code_addr==cs.stages[5].code_addr);

    /* Source may alias the mutable combined object: take a local snapshot. */
    NvnProgram before=*st.program;
    bind_program_stages(&g_comb_prog,3);
    CHECK(st.program==&g_comb_prog && !memcmp(&before,&g_comb_prog,sizeof before));
    bind_program_stages(&g_comb_prog,0);
    CHECK(st.program==&g_comb_prog && !memcmp(&before,&g_comb_prog,sizeof before));

    /* Selective null unbind preserves compute and other graphics stages. */
    recorded_bind(NULL,2); CHECK(st.program->stage_mask==33);
    NvnShaderStage empty={0}; CHECK(!memcmp(&st.program->stages[1],&empty,sizeof empty));
    CHECK(st.program->stages[0].code_addr==vs.stages[0].code_addr);
    CHECK(st.program->stages[5].code_addr==cs.stages[5].code_addr);
    recorded_bind(NULL,32); CHECK(st.program->stage_mask==1);
    CHECK(!memcmp(&st.program->stages[5],&empty,sizeof empty));
    recorded_bind(NULL,1); CHECK(st.program==NULL);
    reset(); recorded_bind(&vf,3); recorded_bind(&cs,32); recorded_bind(NULL,0);
    CHECK(st.program==NULL); /* legacy null/zero unbind clears all six */
    recorded_bind(&vf,3); recorded_bind(NULL,0x3f); CHECK(st.program==NULL);

    /* Binding a program without the selected stage explicitly clears it. */
    reset(); recorded_bind(&vf,3); recorded_bind(&vs,2);
    CHECK(st.program->stage_mask==1 && st.program->stages[0].code_addr==vf.stages[0].code_addr);
    CHECK(!memcmp(&st.program->stages[1],&empty,sizeof empty));
    recorded_bind(&fs,0); CHECK(st.program->stage_mask==3); /* zero uses source mask, preserves VS */
    CHECK(st.program->stages[0].code_addr==vf.stages[0].code_addr);
    recorded_bind(NULL,3); CHECK(st.program==NULL);
    printf("{\"checks\":%u,\"recorder_calls\":%u,\"passed\":true}\n",checks,records);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/shared-program-binding")
    args = parser.parse_args()
    out = args.output.resolve()
    if not args.clang or not out.is_relative_to(ROOT / "build"):
        parser.error("Host Clang and ignored build output required")
    out.mkdir(parents=True, exist_ok=True)
    before = {name: sha(ROOT / name) for name in INPUTS}
    header = (ROOT / INPUTS[2]).read_text()
    definitions = re.search(r"typedef struct NvnShaderStage\s*\{.*?\}\s*NvnProgram\s*;", header, re.S)
    command = re.search(r"typedef struct\s*\{\s*uint32_t op, size;.*?\}\s*CmdRec\s*;", header, re.S)
    host = re.search(r"^#define HOST\(T, guest\).+$", header, re.M)
    enum = re.search(r"enum\s*\{\s*CMD_END\s*=\s*0,.*?\};", header, re.S)
    if not all((definitions, command, host, enum)):
        raise RuntimeError("Production type/macro/command definitions changed")
    executor = (ROOT / INPUTS[1]).read_text()
    declaration = re.search(r"^static NvnProgram g_comb_prog;", executor, re.M)
    if not declaration:
        raise RuntimeError("Production combined program storage missing")
    helper = function(executor, "static void bind_program_stages(")
    recorder = function((ROOT / INPUTS[0]).read_text(), "NVN(nvnCommandBufferBindProgram)")
    # Exact command-handler connection is also checked, without compiling Vulkan.
    if not re.search(r"case CMD_BIND_PROGRAM:\s*bind_program_stages\(\(NvnProgram \*\)\(uintptr_t\)r->x\[0\],\s*\(uint32_t\)r->x\[1\]\);\s*break;", executor):
        raise RuntimeError("Production executor does not dispatch the recorded stage mask")
    source = out / "program_binding.c"
    source.write_text("/* Extracted production types/functions; see source hashes in verification.json. */\n"
                      + "#include <stdint.h>\n" + definitions.group(0) + "\n" + command.group(0)
                      + "\n" + enum.group(0) + "\n" + host.group(0) + "\n" + PRELUDE
                      + "\n" + declaration.group(0) + "\n" + helper + "\n" + recorder + "\n" + TESTS)
    report = {"schema": 1, "verified_utc": datetime.now(timezone.utc).isoformat(), "source_sha256": before,
              "extracted_harness_sha256": sha(source), "device_actions": False, "game_inputs": False,
              "scope": "actual type/helper/recorder extraction with minimal host record callbacks; no GPU execution",
              "variants": {}}
    for name, sanitized in (("optimized", False), ("sanitized", True)):
        runner = out / name
        flags = ["-std=c11", "-O1" if sanitized else "-O2", "-g", "-Wall", "-Wextra", "-Werror"]
        if sanitized:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        build = subprocess.run([args.clang, *flags, str(source), "-o", str(runner)], capture_output=True, text=True, timeout=60)
        (out / f"{name}-compile.log").write_text(build.stdout + build.stderr)
        if build.returncode:
            raise RuntimeError(f"{name}: compilation failed; see ignored compile log")
        run = subprocess.run([str(runner)], capture_output=True, text=True, timeout=30)
        (out / f"{name}-run.log").write_text(run.stdout + run.stderr)
        if run.returncode:
            raise RuntimeError(f"{name}: check failed; see ignored run log")
        result = json.loads(run.stdout)
        report["variants"][name] = {"runner_sha256": sha(runner), "result": result, "exit": run.returncode}
        print(f"{name}: {result['checks']} checks pass", flush=True)
    if before != {name: sha(ROOT / name) for name in INPUTS}:
        raise RuntimeError("Production inputs changed during verification; rerun against a stable snapshot")
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
