#!/usr/bin/env python3
"""Test actual alias backing/page inventory and NPDM resources without games/device."""
import hashlib,json,subprocess,shutil,shlex
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 out=ROOT/'build/physical-memory';out.mkdir(parents=True,exist_ok=True)
 paths=[ROOT/'ports/shared-android/runtime/physical_memory.c',ROOT/'ports/shared-android/runtime/physical_memory.h',ROOT/'tests/physical_memory_test.c',Path(__file__).resolve()]
 kernel=ROOT/'ports/shared-android/runtime/kernel.c';text=kernel.read_text()
 def body(signature):
  start=text.index(signature);at=text.index('{',start);end=at+1;depth=1
  while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
  return text[start:end]
 declarations='typedef struct {uint64_t addr,size;uint32_t type,perm;} Region;\nstatic pthread_mutex_t memory_lock=PTHREAD_MUTEX_INITIALIZER;\nstatic Region regions[1024];static int nregions;static uint64_t heap_size;\n'
 names=['int kernel_data_regions(', 'static void region_add_unlocked(', 'static void region_remove_unlocked(', 'static void region_add(', 'static void region_remove(', 'static int physical_conflict(', 'static int physical_owned_overlap(', 'static int physical_backing(', 'static int prepare_alias_write(', 'int kernel_configure_system_resources(', 'static uint64_t physical_budget(', 'static void reset_physical_memory(', 'static uint32_t set_heap_size(', 'static int physical_memory_info(', 'static void query_memory_unlocked(', 'static void query_memory(']
 (out/'physical_kernel_bodies.h').write_text(declarations+'\n'.join(body(n) for n in names)+'\n')
 paths+=[kernel,kernel.with_suffix('.h'),ROOT/'tests/physical_kernel_test.c',ROOT/'ports/shared-android/runtime/main.c']
 hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
 def run(cmd,log):
  p=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=60);(out/log).write_text(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(log+': '+p.stdout+p.stderr)
  return p.stdout.strip()
 common=['-std=c11','-Wall','-Wextra','-Werror','-I',paths[0].parent,paths[0],paths[2]];result={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  for case,test in [('inventory',paths[2]),('kernel',ROOT/'tests/physical_kernel_test.c')]:
   args=[*common];args[-1]=test;binary=out/(mode+'-'+case);run(['clang',*args,'-I',out,*flags,'-pthread','-o',binary],mode+'-'+case+'-compile.log');result[mode+'-'+case]=run([binary],mode+'-'+case+'-run.log')
 ndk=Path.home()/'android-sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang'
 run([ndk,*common,'-O2','-ffixed-x18','-Wl,--no-undefined','-o',out/'android-test'],'android-link.log')
 overlay=out/'production/runtime'
 for tree in [ROOT/'work/shared-android/runtime',ROOT/'ports/shared-android/runtime']:
  for f in tree.rglob('*'):
   if f.is_file() and f.suffix in ('.h','.inc'):
    dest=overlay/f.relative_to(tree);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,dest)
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text())
 for name in ['kernel.c','main.c']:
  source=ROOT/'ports/shared-android/runtime'/name;target=overlay/name;shutil.copyfile(source,target)
  row=next(r for r in rows if r['file'].endswith('/runtime/'+name));args=row.get('arguments') or shlex.split(row['command']);command=[args[0],'-I'+str(overlay),'-I'+str(ROOT/'native')];i=1
  while i<len(args):
   if args[i] in ('-o','-MF','-MT','-MQ'):i+=2;continue
   if args[i] in ('-MD','-MMD','-MP'):i+=1;continue
   arg=args[i].replace(str(ROOT/'work/shared-android/runtime'),str(overlay))
   if args[i]==row['file']:arg=str(target)
   command.append(arg);i+=1
  run([*command,'-o',out/(name+'.o')],name+'-production-compile.log')
 report={'state':'passed','host':result,'android_link':True,'whole_kernel_and_main_android_compile':True,'source_sha256':hashes,'scope':'Controlled callback-failure rollback is tested only for the synthetic failure-atomic callback; production validated-owned syscall failures are terminal. Actual page inventory and Linux owned reservation backing; negative bounds/overlap, idempotent partial maps, partial unmap, zero remap and failure rollback; no guest execution'}
 (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
