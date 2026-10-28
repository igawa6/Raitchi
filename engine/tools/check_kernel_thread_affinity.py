#!/usr/bin/env python3
"""Asset-free logical core-mask and preserved KThread prefix checks."""
import hashlib,json,pathlib,re,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];RUNTIME=ROOT/'ports/shared-android/runtime';OUT=ROOT/'build/kernel-thread-affinity'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);h=(RUNTIME/'kernel.h').read_text();thread=h[h.index('struct KThread {'):h.index('\n};',h.index('struct KThread {'))+3]
 legacy=thread.replace('struct KThread {','struct LegacyKThread {').replace('    // Logical guest scheduling metadata; append to preserve every prior offset.\n    KernelThreadAffinity affinity;\n','')
 assert 'KernelThreadAffinity affinity;' in thread and 'affinity' not in legacy
 enums=h[h.index('typedef enum { KOBJ_THREAD'):h.index('typedef KObject KEvent;')]
 pre='#include <stdbool.h>\n#include <stddef.h>\n#include <pthread.h>\n#include <setjmp.h>\n#include "runtime/aot_cpu.h"\n#include "kernel_thread_affinity.h"\ntypedef struct KThread KThread;\n'
 check=pre+enums+thread+'\n'+legacy+'\n'
 fields=['obj','cpu','host','entry','tlr','id','handle','prio','core','cancel','in_sync_wait','is_main','wait_kind','wait_addr','wait_mutex','wait_tag','wait_result','wnext','snext','cond','unwind','started','finished','joined','anext','wait_since','sync_handles','sync_count']
 for f in fields:check+='_Static_assert(offsetof(KThread,'+f+')==offsetof(struct LegacyKThread,'+f+'),"preserved KThread.'+f+'");\n'
 check+='int main(void){return 0;}\n';(OUT/'prefix.c').write_text(check)
 paths=[RUNTIME/'kernel_thread_affinity.c',RUNTIME/'kernel_thread_affinity.h',RUNTIME/'LICENSE-kernel-thread-affinity.txt',RUNTIME/'kernel.h',ROOT/'tests/kernel_thread_affinity_test.c',ROOT/'native/runtime/aot_cpu.h',pathlib.Path(__file__)];pins={str(p.relative_to(ROOT)):sha(p) for p in paths};results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',RUNTIME,ROOT/'tests/kernel_thread_affinity_test.c',RUNTIME/'kernel_thread_affinity.c','-pthread','-o',exe]);results[mode]=run([exe])
 run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror','-I',ROOT/'native','-I',RUNTIME,OUT/'prefix.c','-pthread','-o',OUT/'prefix']);run([OUT/'prefix'])
 assert pins=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Source changed during check'
 report={'state':'passed','results':results,'preserved_KThread_fields':fields,'source_sha256':pins,'scope':'Actual pure guest-state helper under explicit external lock, all4-core masks andspecialideals, erroratomicity, configureddefault and per-core jobqueue regression; compile assertions every priorKThread fieldoffset remains unchanged. No host affinity/performance/scheduler changes or device/game execution.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
