#!/usr/bin/env python3
"""Asset-free actual NVN MapVirtual wrappers and complete-span interval tests."""
import hashlib,json,os,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];NVN=ROOT/'ports/shared-android/runtime/nvn';OUT=ROOT/'build/pool-mapping'
def function(s,name):
 start=s.index(name);i=s.index('{',start);depth=1;end=i+1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]+'\n'
def main():
 OUT.mkdir(parents=True,exist_ok=True);source=(NVN/'objects.c').read_text();names=['NVN(nvnMemoryPoolInitialize)','static void pool_destroy(','NVN(nvnMemoryPoolFinalize)','bool nvn_pool_resolve_span(','NVN(nvnMemoryPoolMapVirtual)'];(OUT/'pool_mapping_production.inc').write_text('#include "nvn/command_pool_stats.h"\n'+''.join(function(source,n)for n in names));paths=[NVN/'objects.c',NVN/'nvn.h',NVN/'pool_mapping.c',NVN/'pool_mapping.h',NVN/'command_pool_stats.c',NVN/'command_pool_stats.h',NVN/'command_recording.h',NVN/'command_lifetime.h',NVN/'command_body_arena.h',ROOT/'tests/pool_mapping_test.c',pathlib.Path(__file__)];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths};results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;cmd=[shutil.which('clang'),'-std=gnu11','-Wall','-Wextra','-Werror',*flags,'-I',ROOT/'native','-I',NVN.parent,'-I',OUT,'-I',ROOT/'work/shared-android/runtime/nvn',ROOT/'tests/pool_mapping_test.c',NVN/'pool_mapping.c',NVN/'command_pool_stats.c','-pthread','-o',exe]
  for suffix,args in [('build',cmd),('run',[exe])]:
   env=dict(os.environ)
   if mode=='sanitized' and suffix=='run':env['ASAN_OPTIONS']='detect_leaks=1:halt_on_error=1';env['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1'
   p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60,env=env);(OUT/f'{mode}-{suffix}.log').write_text(p.stdout+p.stderr)
   if p.returncode:raise RuntimeError(p.stdout+p.stderr)
   if suffix=='run':results[mode]=p.stdout.strip()
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}
 report={'state':'passed','results':results,'source_sha256':pins,'scope':'Actual NVN memory-pool wrappers and CPU span mapping. command_lifetime is explicitly OFF; registration hooks are test stand-ins with controllable rejection to verify wrapper rollback, including leak-checked virtual mapping cleanup and no destroy callback for an unregistered pool. Also atomic multi-request failure, partial remap/unmap, contiguous joins, holes/discontiguous rejection, queue-ordered metadata destruction, overflow/resource limits, randomized byte model and concurrent publication. No synthetic GPU VA or device claims.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
