#!/usr/bin/env python3
"""Bounded default-OFF command-pool lifecycle diagnostics; no retirement."""
import os
import hashlib,json,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];NVN=ROOT/'ports/shared-android/runtime/nvn';OUT=ROOT/'build/command-pool-stats'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 OUT.mkdir(parents=True,exist_ok=True);source=NVN/'command_pool_stats.c';header=NVN/'command_pool_stats.h';test=ROOT/'tests/command_pool_stats_test.c';shutil.copyfile(source,OUT/'production_command_pool_stats.c');shutil.copyfile(header,OUT/header.name)
 paths=[source,header,test,NVN/'objects.c',NVN/'cmdbuf.c',pathlib.Path(__file__)];pins={str(p.relative_to(ROOT)):sha(p)for p in paths}
 objects=(NVN/'objects.c').read_text();commands=(NVN/'cmdbuf.c').read_text()
 for token in ['nvn_command_pool_note_init(p, p->storage, p->size);','nvn_command_pool_note_finalize(p);','nvn_command_pool_note_destroy(p);','nvn_command_pool_note_handle_finalize(A(1));']:assert objects.count(token)==1
 assert commands.count('nvn_command_pool_note_add(pool, offset, size, effective);')==1
 results={};common=['-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I',OUT,test,'-pthread']
 def run(args,name):
  p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60);(OUT/name).write_text(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(name+': '+p.stdout+p.stderr)
  return p.stdout
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run([shutil.which('clang'),*common,*flags,'-o',exe],mode+'-build.log')
  results[mode]={choice:json.loads(run([exe,choice],mode+'-'+choice+'.log'))for choice in ['off','on']}
 ndk=pathlib.Path(os.environ.get('ANDROID_SDK_ROOT',pathlib.Path.home()/'android-sdk'))/'ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang';run([ndk,*common,'-O2','-ffixed-x18','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',OUT/'android-test'],'android-link.log')
 assert pins=={str(p.relative_to(ROOT)):sha(p)for p in paths},'Sources changed during checks'
 report={'state':'passed','host':results,'android_cross_link_passed':True,'source_sha256':pins,'scope':'Diagnostic metadata only, no retirement. OFF100000rounds uses zero locks/allocations and leaves output untouched; ON generation reuse, API request versus queued destruction, explicit handle-finalize count,4096slot/table64report bounds,4000concurrent lifecycles and overflow qualification. Hook placement checked in actual sources; full recorder/pool wrapper regressions run separately.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
