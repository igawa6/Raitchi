#!/usr/bin/env python3
"""Bounded regression of actual production shader-cache ownership helpers."""
import hashlib,json,shutil,shlex,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];SOURCE=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c';TEST=ROOT/'tests/shader_cache_ownership_test.c';OUT=ROOT/'build/shader-cache-ownership'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);text=SOURCE.read_text()
 program=text[text.index('static Prog *hs_add_locked('):text.index('static void hs_write(')]
 pipeline=text[text.index('static Pipe *hs_pipe_take('):text.index('// a pipeline made in play: its key')]
 (OUT/'ownership.production.h').write_text(program+'\n'+pipeline)
 # Check the actual callers queue only the winning object and free losing candidates.
 assert 'if (fresh) run_translate(translate_program, p);' in text
 assert 'if (winner != p) {\n            free(p);\n            if (!map) free(buf);\n            continue;' in text
 assert 'if (fresh || !winner) run_job(build_pipeline, pp);' in text
 assert 'if (winner != candidate) free(candidate);' in text
 cc=shutil.which('clang');assert cc;results={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/name;run([cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',OUT,TEST,'-pthread','-o',exe]);results[name]=run([exe]);print(name+': '+results[name],flush=True)
 # Compile the whole actual production TU without changing staged sources.
 entries=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text())
 entry=next(e for e in entries if e['file'].endswith('/runtime/nvn/vulkan/exec.c'))
 overlay=OUT/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':entry['file'],'external-contents':str(SOURCE)}]}))
 command=shlex.split(entry['command']);command[command.index('-o')+1]=str(OUT/'exec.android.o');command+=['-ivfsoverlay',str(overlay)]
 p=subprocess.run(command,cwd=entry['directory'],capture_output=True,text=True,timeout=90)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 paths=[SOURCE,TEST,Path(__file__)];report={'schema':1,'scope':'Actual extracted production program/pipeline cache publication helpers with controlled object payloads and queue counters; 32 simultaneous callers forced to observe initial misses, collisions/full-table policy. Callsite owner gating checked in actualsource. Not shaderc, Vulkan driver, memory-pressure or game execution proof.','results':results,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},'android_actual_tu_compiled':True,'extracted_helpers_sha256':hashlib.sha256((program+'\n'+pipeline).encode()).hexdigest()};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
