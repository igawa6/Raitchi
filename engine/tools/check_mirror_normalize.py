#!/usr/bin/env python3
"""Actual MIR admission and GPU recorder/merge, including overlapping320B resources."""
import argparse,hashlib,json,pathlib,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args();out=ROOT/'build/mirror-normalize';out.mkdir(parents=True,exist_ok=True);rt=ROOT/'ports/shared-android/runtime';source=rt/'nvn/vulkan/exec.c';header=source.with_name('mirror_normalize.h');uf=source.with_name('uniform_forward.h');test=ROOT/'tests/mirror_normalize_test.c';fixture=ROOT/'tests/graphics_mirror_test.c';paths=[source,header,uf,test,fixture,pathlib.Path(__file__).resolve()];pins={str(f.relative_to(ROOT)):sha(f)for f in paths};text=source.read_text();start=text.index('#define MIR_SIZE ');end=text.index('// (guest_direct)',start);fragment=text[start:end];start=fragment.index('static void projection_probe_mirror_cb4(');end=fragment.index('\n}\n',start)+3;fragment=fragment[:start]+fragment[end:];(out/'graphics_mirror_fragments.h').write_text(fragment)
 dispatch=text[text.index('static void dispatch_compute('):text.index('// ================================================================== draws')];assert dispatch.index('mirror_normalize_compute(cp->si);')<dispatch.index('VkCommandBuffer cb = vk_cmd();')<dispatch.index('stage_set(')
 draw=text[text.index('static void draw(CmdRec *r, bool indexed)'):text.index('// ================================================================== texture copies')];assert draw.index('mirror_normalize_graphics(pr->vi, pr->fi);')<draw.index('resolve_feedback(pr);')<draw.index('mirror_prepare_graphics(')<draw.index('begin_pass();')<draw.index('stage_set(')
 def run(cmd,name,cwd=None):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,cwd=cwd,timeout=120);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 host={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,'-I',header.parent,test,'-pthread','-o',exe],name+'-build.log');host[name]=json.loads(run([exe],name+'-run.log').splitlines()[-1])
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/runtime/nvn/vulkan/exec.c'));overlay=out/'overlay.json';owned=[source,header,uf,rt/'nvn/vulkan/vk.h',rt/'nvn/maxwell/shader.h'];overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/f.relative_to(rt)),'external-contents':str(f)}for f in owned]}));cmd=shlex.split(row['command']);obj=out/'exec-android.o';cmd[cmd.index('-o')+1]=str(obj);run([*cmd,'-ivfsoverlay',overlay],'android-production-compile.log',row['directory']);android={'passed':True,'object_sha256':sha(obj)}
 assert pins=={str(f.relative_to(ROOT)):sha(f)for f in paths},'Source changed during checks'
 receipt={'state':'passed','host':host,'android':android,'source_sha256':pins,'scope':'Actual production MIR allocator/merge/queued GPU recorder and admission header. Fresh overlap only synchronizes outside lock/pass, preserves preflush CPU-dirty words and authoritative completed GPU segments, CPU holes, chained/multiowner unions, and compute/VS+FS preflight before tables. Contained/readonly requests do not flush. Ambiguous pre-existing active overlap fails. Disabled path tested; actual default-on gate covered by check_uniform_forward.py. No device performance claim.'};(out/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
