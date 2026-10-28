#!/usr/bin/env python3
"""Actual kernel logical-core wiring, typed handles and concurrent self updates."""
import hashlib,json,pathlib,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
 source=ROOT/'ports/shared-android/runtime/kernel.c';s=source.read_text();out=ROOT/'build/kernel-thread-core-integration';out.mkdir(parents=True,exist_ok=True)
 def function(sig):
  start=s.index(sig);at=s.index('{',start)+1;depth=1
  while depth:depth+=(s[at]=='{')-(s[at]=='}');at+=1
  return s[start:at]+'\n'
 funcs=['int kernel_configure_thread_core(', 'static uint32_t thread_core_get(', 'static uint32_t thread_core_set(', 'static uint32_t thread_current_core(', 'static KThread *thread_new(']
 (out/'kernel_thread_core_production.h').write_text(''.join(function(x) for x in funcs))
 for sig in ['void kernel_reset(', 'void kernel_init(']:assert 'process_default_core = 0;' in function(sig)
 for needle in ['thread_core_get((uint32_t)x[2]', 'thread_core_set((uint32_t)x[0]', 'x[0] = thread_current_core()', 'prio, -2, &result)', 'case 0: v = PROCESS_CORE_MASK;']:assert needle in s
 paths=[source,ROOT/'ports/shared-android/runtime/kernel_thread_affinity.c',ROOT/'ports/shared-android/runtime/kernel_thread_affinity.h'];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths};reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/name;cmd=['clang','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',str(out),'-I',str(paths[1].parent),str(ROOT/'tests/kernel_thread_core_integration_test.c'),str(paths[1]),'-pthread','-o',str(binary)]
  for step,args in [('compile',cmd),('run',[str(binary)])]:
   r=subprocess.run(args,capture_output=True,text=True,timeout=60);(out/f'{name}-{step}.log').write_text(r.stdout+r.stderr)
   if r.returncode:raise RuntimeError(r.stdout+r.stderr)
   if step=='run':reports[name]=json.loads(r.stdout)
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
 report={'state':'passed','host':reports,'source_sha256':pins,'scope':'Actual kernel helper bodies/create path plus pure affinity helper, typed/pseudo handles, error precedence, no publication on invalid create, concurrent self setters. No device/full build.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
