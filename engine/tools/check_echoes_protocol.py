#!/usr/bin/env python3
"""Asset-free TIPC/HIPC and audio revision checks using the actual shared implementations."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import shlex
import subprocess

ROOT=Path(__file__).resolve().parents[1]
IPC=ROOT/"ports/shared-android/runtime/ipc.c"
AUDIO=ROOT/"ports/shared-android/runtime/services/audio.c"
HEADER=ROOT/"ports/shared-android/runtime/ipc.h"

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang",default=shutil.which("clang"))
    parser.add_argument("--output",type=Path,default=ROOT/"build/echoes-protocol")
    parser.add_argument("--android",action="store_true",help="Compile both actual overlays with the existing staged NDK compile flags into this check's directory")
    args=parser.parse_args()
    if not args.clang:parser.error("Host Clang required")
    out=args.output.resolve()
    if not out.is_relative_to(ROOT/"build"):parser.error("Output must remain under ignored build directory")
    out.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(IPC,out/"production_ipc.c")
    shutil.copyfile(HEADER,out/"ipc.h")
    (out/"kernel.h").write_text('''#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint64_t tpidrro; } Cpu;
enum { KOBJ_THREAD=1,KOBJ_EVENT=2,KOBJ_SESSION=3 };
typedef struct { int type; } KObject;
KObject *handle_get(uint32_t);
uint32_t kernel_new_handle(KObject *);
''')
    (out/"hle.h").write_text('''#pragma once
#include <stdio.h>
#include "kernel.h"
#define HLE_CAT2(a,b) a##b
#define HLE_CAT(a,b) HLE_CAT2(a,b)
#define LOG(...) ((void)0)
''')
    audio=AUDIO.read_text();begin=audio.index("static void request_update(");end=audio.index("\nstatic void renderer_open_output(",begin)
    body=audio[begin:end].strip()
    if not body.endswith("}"):raise RuntimeError("Audio RequestUpdate extraction boundary changed")
    (out/"production_audio_update.inc").write_text(body+"\n")
    device_begin=audio.index("static void audio_output_name(");device_end=audio.index("\ntypedef struct AudioOut",device_begin)
    (out/"production_audio_device.inc").write_text(audio[device_begin:device_end]+"\n")
    request_begin=audio.index("// A bounded snapshot of the fixed audren:u request bytes")
    request_end=audio.index('\nSERVICE("audren:u"',request_begin)
    (out/"production_audio_request_summary.inc").write_text(audio[request_begin:request_end]+"\n")
    results={}
    for name in ("ipc","audio_revision","audio_device","audio_request_summary"):
        source=ROOT/f"tests/echoes_{name}_test.c"
        results[name]={}
        for variant,flags in (("optimized",["-O2"]),("sanitized",["-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer"])):
            binary=out/f"{name}-{variant}"
            cmd=[args.clang,"-std=c11","-D_GNU_SOURCE","-Wall","-Wextra","-Werror","-Wno-unused-parameter",*flags,"-I",str(out),str(source),"-pthread","-o",str(binary)]
            built=subprocess.run(cmd,check=True,text=True,capture_output=True,timeout=30)
            (out/f"{name}-{variant}-build.log").write_text(built.stdout+built.stderr)
            ran=subprocess.run([str(binary)],check=True,text=True,capture_output=True,timeout=15)
            (out/f"{name}-{variant}-run.log").write_text(ran.stdout+ran.stderr)
            results[name][variant]={"passed":True,"output":ran.stdout.strip()};print(variant+": "+ran.stdout.strip(),flush=True)
    sources=[IPC,AUDIO,HEADER,Path(__file__),ROOT/"tests/echoes_ipc_test.c",ROOT/"tests/echoes_audio_revision_test.c",ROOT/"tests/echoes_audio_device_test.c",ROOT/"tests/echoes_audio_request_summary_test.c"]
    android=[]
    if args.android:
        database=ROOT/"build/shared-android/native/compile_commands.json"
        rows=json.loads(database.read_text())
        for source in [IPC,AUDIO]:
            suffix="/runtime/"+str(source.relative_to(ROOT/"ports/shared-android/runtime"))
            matches=[row for row in rows if row["file"].endswith(suffix)]
            if len(matches)!=1:raise RuntimeError("Missing unique staged NDK compile entry: "+suffix)
            row=matches[0];original=shlex.split(row["command"]);command=[];index=0
            while index<len(original):
                item=original[index]
                if item in ("-o","-MF","-MT","-MQ"):index+=2;continue
                if item in ("-MD","-MMD","-MP") or item==row["file"]:index+=1;continue
                command.append(item);index+=1
            obj=out/(source.stem+"-android.o")
            command.extend(["-I",str(ROOT/"work/shared-android/runtime/services"),str(source),"-o",str(obj)])
            built=subprocess.run(command,cwd=row["directory"],check=True,capture_output=True,text=True,timeout=60)
            (out/(source.stem+"-android-build.log")).write_text(built.stdout+built.stderr)
            android.append({"source":str(source.relative_to(ROOT)),"object_sha256":digest(obj),"passed":True})
            print("Android actual overlay compiled: "+source.name,flush=True)
    report={"schema":1,"scope":"Actual entire production IPC codec + pinned actual ipc.h, controlled kernel handle/Cpu stand-ins; verbatim production audio RequestUpdate with controlled mixer/voice/effect stand-ins plus actual output-device list/name handler with controlled event creation. Optional actual-overlay NDK compilation uses existing staged real headers/dependencies. No devices, real game execution, audio playback or kernel lifecycle qualification.","source_sha256":{str(p.relative_to(ROOT)):digest(p) for p in sources},"audio_function_sha256":hashlib.sha256(body.encode()).hexdigest(),"results":results,"android_compile":android,"device_actions":False}
    (out/"verification.json").write_text(json.dumps(report,indent=2)+"\n")

if __name__=="__main__":main()
