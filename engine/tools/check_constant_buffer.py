#!/usr/bin/env python3
"""Bounded C1 staging helper: declared reads and guard-page regression."""
import hashlib,json,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];RT=ROOT/'ports/shared-android/runtime';OUT=ROOT/'build/constant-buffer'
def main():
 OUT.mkdir(parents=True,exist_ok=True);paths=[RT/'nvn/constant_buffer.c',RT/'nvn/constant_buffer.h',ROOT/'tests/constant_buffer_test.c',pathlib.Path(__file__)];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths};results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;cmd=[shutil.which('clang'),'-std=gnu11','-Wall','-Wextra','-Werror',*flags,'-I',RT,ROOT/'tests/constant_buffer_test.c',RT/'nvn/constant_buffer.c','-o',exe]
  for kind,args in [('build',cmd),('run',[exe])]:
   p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60);(OUT/f'{mode}-{kind}.log').write_text(p.stdout+p.stderr)
   if p.returncode:raise RuntimeError(p.stdout+p.stderr)
   if kind=='run':results[mode]=p.stdout.strip()
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}
 report={'state':'passed','results':results,'source_sha256':pins,'scope':'Pure C1 staging only. Actual20B source ends at PROT_NONE guard page, shader extent256/65536, exact source copy and zero padding; empty declarations, bounded prefix caps, pointer/size overflow, overlap and destination canaries. No guest mapping/readability validation or Vulkan/device claims.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
