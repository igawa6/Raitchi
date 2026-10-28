#!/usr/bin/env python3
"""Validate explicit late-fatal NVN proc diagnostics; no invented NVN implementations or device actions."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/"ports/shared-android/runtime/hle/nvn_boot.c"

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android",action="store_true")
    args=parser.parse_args()
    out=ROOT/"build/nvn-proc-diagnostics";out.mkdir(parents=True,exist_ok=True)
    copied=out/"hle/production_nvn_boot.c";copied.parent.mkdir(exist_ok=True);shutil.copyfile(SOURCE,copied)
    (out/"hle.h").write_text('''#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "runtime/aot_cpu.h"
typedef void (*HostFn)(Cpu *);
HostFn hle_lookup_impl(const char *);
uint64_t hle_register(const char *,HostFn);
const char *hle_name_at(uint64_t);
#define HLE_FUNC(name,fn) static void fn(Cpu *c)
''')
    results={}
    compiler=shutil.which("clang")
    if not compiler:raise RuntimeError("Host Clang required")
    for variant,flags in (("optimized",["-O2"]),("sanitized",["-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer"])):
        binary=out/variant
        built=subprocess.run([compiler,"-std=c11","-D_GNU_SOURCE","-Wall","-Wextra","-Werror",*flags,"-I",str(ROOT/"native"),"-I",str(out),"-I",str(out/"hle"),str(ROOT/"tests/nvn_proc_diagnostic_test.c"),"-o",str(binary)],capture_output=True,text=True,timeout=30)
        (out/(variant+"-build.log")).write_text(built.stdout+built.stderr)
        built.check_returncode()
        ran=subprocess.run([str(binary)],check=True,capture_output=True,text=True,timeout=10)
        results[variant]=ran.stdout.strip();print(variant+": "+ran.stdout.strip(),flush=True)
    android={}
    if args.android:
        rows=json.loads((ROOT/"build/shared-android/native/compile_commands.json").read_text())
        row=next(x for x in rows if x["file"].endswith("/runtime/hle/nvn_boot.c"))
        command=shlex.split(row["command"]);index=command.index("-o");obj=out/"nvn_boot-android.o";command[index+1]=str(obj)
        command[-1]=str(SOURCE);command.extend(["-I",str(ROOT/"work/shared-android/runtime/hle")])
        # Quoted ../hle.h resolves through one consistent real runtime header tree.
        built=subprocess.run(command,cwd=row["directory"],capture_output=True,text=True,timeout=60)
        (out/"android-build.log").write_text(built.stdout+built.stderr)
        built.check_returncode()
        android={"passed":True,"object_sha256":sha(obj)};print("Actual NVN boot overlay Android ARM64 compilation passed",flush=True)
    files=[SOURCE,ROOT/"tests/nvn_proc_diagnostic_test.c",Path(__file__),ROOT/"native/runtime/aot_cpu.h"]
    report={"schema":1,"scope":"Actual entire NVN bootstrap/device-proc overlay with actual Cpu ABI; controlled HLE registry/fatal path. Checks lookup-vs-invocation behavior, not NVN getter semantics or graphics support.","source_sha256":{str(p.relative_to(ROOT)):sha(p) for p in files},"results":results,"android_compile":android,"device_actions":False}
    (out/"verification.json").write_text(json.dumps(report,indent=2)+"\n")

if __name__=="__main__":main()
