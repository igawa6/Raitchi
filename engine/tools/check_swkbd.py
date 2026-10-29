#!/usr/bin/env python3
"""Actual SDK ShowKeyboard HLE with bounded production Unicode conversion and controlled guest/UI boundaries."""
import argparse,hashlib,json,shlex,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];RUNTIME=ROOT/'ports/shared-android/runtime';OUT=ROOT/'build/swkbd'
def run(cmd,name,cwd=ROOT):
 p=subprocess.run(list(map(str,cmd)),cwd=cwd,capture_output=True,text=True,timeout=90);(OUT/(name+'.log')).write_text(p.stdout+p.stderr)
 if p.returncode:raise RuntimeError(name+': '+p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--android',action='store_true');args=parser.parse_args();OUT.mkdir(parents=True,exist_ok=True)
 source=RUNTIME/'hle/swkbd.c';test=ROOT/'tests/swkbd_test.c';paths=[source,test,RUNTIME/'ui/keyboard_text.h',RUNTIME/'ui/menu.mm',RUNTIME/'ui/osk.mm',ROOT/'tests/keyboard_text_test.cpp',Path(__file__)];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}
 staged=OUT/'host/runtime/hle/swkbd.c';staged.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,staged)
 (staged.parent.parent/'ui').mkdir(exist_ok=True);shutil.copyfile(RUNTIME/'ui/keyboard_text.h',staged.parent.parent/'ui/keyboard_text.h')
 (staged.parent.parent/'hle.h').write_text('#pragma once\n#include <stdint.h>\ntypedef struct {uint64_t x[32],pc,flags;} Cpu;\n#define HLE_FUNC(symbol,name) static void name(Cpu*c)\n#define ENV(name) ((const char*)0)\n#define LOG(...) ((void)0)\n')
 (staged.parent.parent/'kernel.h').write_text('#pragma once\n#include <stdint.h>\n#include <stdbool.h>\nbool kernel_copy_readable(void*,uint64_t,uint64_t);\nbool kernel_copy_writable(uint64_t,const void*,uint64_t);\n')
 results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
  exe=OUT/mode;run(['clang','-std=c11','-Wall','-Wextra','-Werror',*flags,'-DSWKBD_SOURCE="'+str(staged)+'"',test,'-o',exe],mode+'-build');results[mode]=run([exe],mode+'-run')
 menu=(RUNTIME/'ui/menu.mm').read_text();start=menu.index('extern "C" int menu_keyboard_run(');end=menu.index('\n}',start)+2
 (OUT/'keyboard_run.inc').write_text(menu[start:end])
 osk=(RUNTIME/'ui/osk.mm').read_text();start=osk.index('    if (max_chars <= 0');end=osk.index('\n    // typed on a keyboard',start)
 (OUT/'keyboard_edit.inc').write_text(osk[start:end])
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
  exe=OUT/('ui-'+mode);run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(RUNTIME/'ui'),'-I'+str(OUT),ROOT/'tests/keyboard_text_test.cpp','-o',exe],mode+'-ui-build');results['ui_'+mode]=run([exe],mode+'-ui-run')
 if args.android:
  overlay=OUT/'production/runtime'
  for tree in (ROOT/'work/shared-android/runtime',RUNTIME):
   for path in tree.rglob('*'):
    if path.is_file()and path.suffix in('.h','.inc'):
     target=overlay/path.relative_to(tree);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
  target=overlay/'hle/swkbd.c';target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/runtime/services/am.c'));cmd=row.get('arguments')or shlex.split(row['command']);command=[cmd[0],'-I'+str(overlay),'-I'+str(ROOT/'native')];i=1
  while i<len(cmd):
   if cmd[i]in('-o','-MF','-MT','-MQ'):i+=2;continue
   if cmd[i]in('-MD','-MMD','-MP'):i+=1;continue
   command.append(str(target)if cmd[i]==row['file']else cmd[i].replace(str(ROOT/'work/shared-android/runtime'),str(overlay)));i+=1
  run([*command,'-o',OUT/'swkbd.android.o'],'android-production',row['directory'])
  ui_roots=[{'type':'file','name':str(ROOT/'work/shared-android/runtime/ui'/name),'external-contents':str(RUNTIME/'ui'/name)}for name in('menu.mm','osk.mm','keyboard_text.h')]
  vfs=OUT/'ui-overlay.json';vfs.write_text(json.dumps({'version':0,'roots':ui_roots}))
  for name in('menu.mm','osk.mm'):
   row=next(r for r in rows if r['file'].endswith('/runtime/ui/'+name));cmd=row.get('arguments')or shlex.split(row['command']);cmd=list(cmd);cmd[cmd.index('-o')+1]=str(OUT/(name+'.android.o'))
   run([*cmd,'-I'+str(RUNTIME/'ui'),'-ivfsoverlay',vfs],'android-'+name,row['directory'])
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}
 report={'status':'PASS','host':results,'android_whole_tu':args.android,'android_ui_tus':args.android,'scope':'Actual HLE and bounded stack production Unicode conversion; controlled readable/writable guest spans and UI only. SDK flag-selected UTF8/UTF16 output, extracted production menu initialization and OSK editing, bounded initial snapshots, surrogate/codeunit/capacity truncation, cancelled initial/empty, repeated output, malformed ABI rejection and untouched huge-capacity page residency. No Android UI/device name-entry proof.','source_sha256':pins};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
