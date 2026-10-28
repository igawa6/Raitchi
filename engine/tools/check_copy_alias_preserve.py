#!/usr/bin/env python3
"""Actual copy-alias lifetime regression and full Android renderer compilation."""
import hashlib,json,shlex,shutil,subprocess
from pathlib import Path
from check_storage_image_views import function,struct
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 out=ROOT/'build/copy-alias-preserve';out.mkdir(parents=True,exist_ok=True);runtime=ROOT/'ports/shared-android/runtime';source=runtime/'nvn/vulkan/exec.c';header=runtime/'nvn/vulkan/vk.h';nvn=runtime/'nvn/nvn.h';test=ROOT/'tests/copy_alias_preserve_test.c';paths=[source,header,nvn,test,Path(__file__)];pins={str(p.relative_to(ROOT)):sha(p)for p in paths};text=source.read_text()
 types='typedef void *VmaAllocation;typedef struct {unsigned unused;} TexLayout;typedef struct NvnPool NvnPool;\n'+struct(nvn.read_text(),'typedef struct NvnTexture {','} NvnTexture;')+'\n'+struct(header.read_text(),'typedef struct TexBackend {','} TexBackend;')+'\n';(out/'production_types.inc').write_text(types)
 def extract(text):
  start=text.index('typedef struct { NvnTexture *src, *dst; VkImage simg, dimg; bool read; } CopyAlias;');end=text.index('extern uint32_t vk_view_gen;',start)
  globals_=text[start:end];start=text.index('static CopyAlias aliases[8];');end=text.index('// still valid:',start)
  return globals_+'\n'+text[start:end]+'\n'+'\n'.join(function(text,n)for n in ('alias_valid','alias_drop','alias_copy','alias_before_write','alias_read','alias_catch_up','alias_make'))+'\n'
 (out/'production_alias.inc').write_text(extract(text))
 def run(cmd,log,expect_success=True,cwd=ROOT):
  p=subprocess.run(list(map(str,cmd)),cwd=cwd,capture_output=True,text=True,timeout=90);(out/log).write_text(p.stdout+p.stderr)
  if expect_success and p.returncode:raise RuntimeError(log+': '+p.stdout+p.stderr)
  return p
 reports={};includes=ROOT/'work/las-baseline/build/android/deps/include'
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/mode;run([shutil.which('clang'),'-std=c11','-D_GNU_SOURCE','-DNDEBUG','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',*flags,'-I',out,'-I',includes,test,'-o',binary],mode+'-build.log');reports[mode]=json.loads(run([binary],mode+'-run.log').stdout)
 old=out/'source-before.c';old_failed=None
 if old.exists():
  olddir=out/'prior';olddir.mkdir(exist_ok=True);(olddir/'production_types.inc').write_text(types);(olddir/'production_alias.inc').write_text(extract(old.read_text()));binary=olddir/'test'
  run([shutil.which('clang'),'-std=c11','-D_GNU_SOURCE','-DNDEBUG','-O2','-I',olddir,'-I',includes,test,'-o',binary],'prior-build.log');p=run([binary],'prior-run.log',False);old_failed=p.returncode!=0;assert old_failed,'Original unread-history bug was not reproduced'
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/exec.c'))
 overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/r),'external-contents':str(runtime/r)}for r in ('nvn/vulkan/exec.c','nvn/vulkan/vk.h','nvn/maxwell/shader.h')]}));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(out/'exec-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd,'android-full-tu.log',cwd=row['directory'])
 assert pins=={str(p.relative_to(ROOT)):sha(p)for p in paths},'Source changed during verification'
 report={'schema':1,'scope':'Actual production alias copy/admission/overwrite/read policy with exact texture declarations and controlled Vulkan image-byte storage. NDEBUG optimized+ASan/UBSan; source-before regression when available; whole Android exec.c TU compile. No real GPU/game visual qualification.','tests':reports,'original_source_reproducer_failed':old_failed,'android_full_tu':True,'source_sha256':pins};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
