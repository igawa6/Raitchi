#!/usr/bin/env python3
"""Actual kernel span validation/copy against real protected pages and mapper."""
import hashlib,json,pathlib,re,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
 source=ROOT/'ports/shared-android/runtime/kernel.c';header=source.with_suffix('.h');mapper=source.with_name('physical_memory.c');test=ROOT/'tests/kernel_readable_copy_test.c';out=ROOT/'build/kernel-readable-copy';out.mkdir(parents=True,exist_ok=True);s=source.read_text()
 def body(sig):
  start=s.index(sig);at=s.index('{',start)+1;depth=1
  while depth:depth+=(s[at]=='{')-(s[at]=='}');at+=1
  return s[start:at]+'\n'
 limit=re.search(r'^#define KERNEL_READABLE_COPY_MAX .+$',header.read_text(),re.M).group(0)
 (out/'kernel_readable_copy_production.h').write_text(limit+'\n'+body('static void query_memory_unlocked(')+body('bool kernel_copy_readable(')+body('bool kernel_copy_writable('))
 paths=[source,header,mapper,test,pathlib.Path(__file__).resolve()];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths};reports={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/mode
  for step,args in [('compile',['clang','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',str(out),'-I',str(mapper.parent),str(test),str(mapper),'-pthread','-o',str(binary)]),('run',[str(binary)])]:
   r=subprocess.run(args,capture_output=True,text=True,timeout=60);(out/f'{mode}-{step}.log').write_text(r.stdout+r.stderr)
   if r.returncode:raise RuntimeError(r.stdout+r.stderr)
   if step=='run':reports[mode]=json.loads(r.stdout)
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
 report={'state':'passed','host':reports,'source_sha256':pins,'scope':'Actual query_memory_unlocked/kernel_copy_readable with real physical bitmap, RO/RW/PROT_NONE pages; cross-region and committed alias, prefix+hole no partialwrite, permissions/reserved/bounds/cap/overlap, mapping-race serialization. No device/full build.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
