#!/usr/bin/env python3
"""Actual optional GPU-owned uniform forwarding with a controlled command recorder."""
import argparse,hashlib,json,pathlib,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args();out=ROOT/'build/uniform-forward';out.mkdir(parents=True,exist_ok=True);rt=ROOT/'ports/shared-android/runtime';source=rt/'nvn/vulkan/exec.c';header=source.with_name('uniform_forward.h');test=ROOT/'tests/uniform_forward_test.c';paths=[source,header,source.with_name('mirror_normalize.h'),test,pathlib.Path(__file__).resolve()];pins={str(f.relative_to(ROOT)):sha(f)for f in paths};text=source.read_text();start=text.index('static void mirror_cpu_changes(');end=text.index('\n}\n',start)+3;(out/'uniform_forward.cpu.h').write_text(text[start:end]);
 for func,name in [('mirror_merge_locked','merge'),('mirror_after_dispatch','readback'),('mirror_slot_idle','idle')]:
  start=text.index(('static bool ' if name=='idle' else 'static void ')+func+'(');end=text.index('\n}\n',start)+3;(out/('uniform_forward.'+name+'.h')).write_text(text[start:end])
 assert text.index('mirror_uniform_prepare_graphics(pr->vi, pr->fi);')<text.index('begin_pass();',text.index('mirror_uniform_prepare_graphics(pr->vi, pr->fi);'));assert 'if (mirror_uniform_info(stage, k, r.addr, size, &forwarded)) return forwarded;'in text;assert 'mirror_uniform_explicit_update(r->x[0] + r->x[2], x, r->x[3]);'in text;assert 'uniform_forward_enabled() ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : 0'in text;assert '!uniform_forward_graphics_prepared) renderer_submit();'in text
 def run(cmd,name,cwd=None):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,cwd=cwd,timeout=120);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 host={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,'-I',out,'-I',header.parent,test,'-pthread','-o',exe],name+'-build.log');host[name]={mode:json.loads(run([exe,mode],name+'-'+mode+'-run.log'))for mode in ('off','on','default')}
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/runtime/nvn/vulkan/exec.c'));overlay=out/'overlay.json';owned=[source,header,source.with_name('mirror_normalize.h'),rt/'nvn/vulkan/vk.h',rt/'nvn/maxwell/shader.h'];overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/f.relative_to(rt)),'external-contents':str(f)}for f in owned]}));cmd=shlex.split(row['command']);obj=out/'exec-android.o';cmd[cmd.index('-o')+1]=str(obj);run([*cmd,'-ivfsoverlay',overlay],'android-production-compile.log',row['directory']);android={'passed':True,'object_sha256':sha(obj)}
 assert pins=={str(f.relative_to(ROOT)):sha(f)for f in paths},'Source changed during checks'
 receipt={'state':'passed','host':host,'android':android,'source_sha256':pins,'scope':'Actual forwarding header and actual CPU-diff producer with synthetic owned guest/MIR/transient buffers and Vulkan recorder. Tests aligned direct reads and GPU composition across partial/disjoint owners and CPU holes; shared graphics+compute cameras, fresh GPU results, same-submission CPU edits and same-value overrides with actual readback/merge; ambiguous ownership/word alignment/overflow failures, generation/seq rejection, lifetime last_use and fresh-process default ON with explicit0 escape. No device correctness or performance claim.'};(out/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
