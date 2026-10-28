#!/usr/bin/env python3
"""Actual snapshot freshness policy regression, plus whole Android renderer compile."""
import hashlib,json,shlex,shutil,subprocess
from pathlib import Path
from check_storage_image_views import function
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 out=ROOT/'build/feedback-stencil-invalidation';out.mkdir(parents=True,exist_ok=True)
 runtime=ROOT/'ports/shared-android/runtime';source=runtime/'nvn/vulkan/exec.c';test=ROOT/'tests/feedback_stencil_invalidation_test.c';paths=[source,test,Path(__file__),runtime/'nvn/maxwell/shader.h',runtime/'nvn/vulkan/vk.h'];pins={str(p.relative_to(ROOT)):sha(p)for p in paths};text=source.read_text()
 start=text.index('static NvnTexture *snap_tex[8];');end=text.index('\ntypedef struct { NvnTexture *key;',start)
 fragment=text[start:end]+'\n'+function(text,'snapshot_draw_writes_depth_stencil')+'\n'+function(text,'snapshot_self_write_reuse')+'\n';(out/'production_snapshot_helpers.inc').write_text(fragment)
 assert 'bool self_write = snapshot_self_write_reuse(pr, t);' in text
 assert 'if (st.rt.depth && snapshot_draw_writes_depth_stencil()) snapshots_invalidate(st.rt.depth);' in text
 def run(cmd,log,cwd=ROOT):
  p=subprocess.run(list(map(str,cmd)),cwd=cwd,capture_output=True,text=True,timeout=90);(out/log).write_text(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(log+': '+p.stdout+p.stderr)
  return p.stdout.strip()
 reports={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/mode;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,'-I',runtime,test,'-o',binary],mode+'-build.log');reports[mode]=json.loads(run([binary],mode+'-run.log'));assert reports[mode]['state']=='passed'
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/exec.c'))
 overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/r),'external-contents':str(runtime/r)}for r in ('nvn/vulkan/exec.c','nvn/vulkan/vk.h','nvn/maxwell/shader.h')]}))
 cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(out/'exec-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd,'android-full-tu.log',row['directory'])
 assert pins=={str(p.relative_to(ROOT)):sha(p)for p in paths},'Source changed during checks'
 report={'schema':1,'scope':'Actual production snapshot invalidation and self-write reuse helpers with controlled renderer state, including stencil-only producer and depth-first/stencil-second binding deduplication counterexamples. Full Android exec.c TU compile; no GPU copy/content or visual qualification.','tests':reports,'android_full_tu':True,'source_sha256':pins};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
