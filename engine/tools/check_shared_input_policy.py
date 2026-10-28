#!/usr/bin/env python3
"""Check the actual physical-ZR policy and controller-finger implementation without SDL/devices."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/"ports/shared-android/runtime/hle/input.c"

def braced(text,start):
    begin=text.index("{",start);depth=0
    for index in range(begin,len(text)):
        if text[index]=="{":depth+=1
        elif text[index]=="}":
            depth-=1
            if not depth:return text[start:index+1]
    raise RuntimeError("Missing production block boundary")

def main():
    out=ROOT/"build/shared-input-policy";out.mkdir(parents=True,exist_ok=True)
    source=SOURCE.read_text()
    enum_start=source.index("enum {");enum_end=source.index("};",enum_start)+2
    function=braced(source,source.index("static bool pad_finger(int32_t *x, int32_t *y) {"))
    block=braced(source,source.index("if (title_adapter_selected()->id == TITLE_EEVEE) {",source.index("static void fill_state(")))
    (out/"production_input_policy.inc").write_text(source[enum_start:enum_end]+"\n"+function+"\n")
    (out/"production_input_button_block.inc").write_text(block+"\n")
    results={}
    compiler=shutil.which("clang")
    if not compiler:raise RuntimeError("Host Clang required")
    for variant,flags in (("optimized",["-O2"]),("sanitized",["-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer"])):
        binary=out/variant
        subprocess.run([compiler,"-std=c11","-Wall","-Wextra","-Werror",*flags,"-I",str(ROOT/"native"),"-I",str(out),str(ROOT/"tests/shared_input_policy_test.c"),"-o",str(binary)],check=True,capture_output=True,text=True,timeout=30)
        ran=subprocess.run([str(binary)],check=True,capture_output=True,text=True,timeout=10)
        results[variant]=ran.stdout.strip();print(variant+": "+ran.stdout.strip(),flush=True)
    paths=[SOURCE,Path(__file__),ROOT/"tests/shared_input_policy_test.c",ROOT/"native/adapters/title_adapter.h"]
    report={"schema":1,"scope":"Verbatim changed fill_state button-policy block and actual pad_finger body with actual production button enum/title header. Controlled input/time; no complete SDL HID pipeline or device claims.","source_sha256":{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},"results":results,"device_actions":False}
    (out/"verification.json").write_text(json.dumps(report,indent=2)+"\n")

if __name__=="__main__":main()
