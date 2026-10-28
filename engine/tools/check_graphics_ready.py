#!/usr/bin/env python3
"""First-use graphics wait without inline compilation or skipped building draws."""
import argparse,hashlib,json,pathlib,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args();out=ROOT/'build/graphics-ready';out.mkdir(parents=True,exist_ok=True);rt=ROOT/'ports/shared-android/runtime';source=rt/'nvn/vulkan/exec.c';test=ROOT/'tests/graphics_ready_test.c';uf=source.with_name('uniform_forward.h');norm=source.with_name('mirror_normalize.h');paths=[source,uf,norm,test,pathlib.Path(__file__).resolve()];pins={str(f.relative_to(ROOT)):sha(f)for f in paths};text=source.read_text();start=text.index('static int graphics_wait_ready(');end=text.index('\n}\n',start)+3;(out/'graphics_ready.production.h').write_text(text[start:end]);draw=text[text.index('static void draw_(CmdRec *r, bool indexed)'):text.index('// ================================================================== texture copies')];assert draw.count('graphics_wait_ready(&pr->state)')==2;assert draw.count('graphics_wait_ready(&pp->state)')==1;assert 'storing graphics pipeline failed; refusing to discard shader writes'in draw;assert 'if (sync_shaders() && !bg) { fn(arg); return; }'in text;assert 'queue_push(&g_jobs, fn, arg, bg);'in text and 'queue_push(&g_tjobs, fn, arg, bg);'in text
 def run(cmd,name,cwd=None):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,cwd=cwd,timeout=120);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 host={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,test,'-pthread','-o',exe],name+'-build.log');host[name]=json.loads(run([exe],name+'-run.log'))
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/runtime/nvn/vulkan/exec.c'));overlay=out/'overlay.json';owned=[source,uf,norm,rt/'nvn/vulkan/vk.h',rt/'nvn/maxwell/shader.h'];overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/f.relative_to(rt)),'external-contents':str(f)}for f in owned]}));cmd=shlex.split(row['command']);obj=out/'exec-android.o';cmd[cmd.index('-o')+1]=str(obj);run([*cmd,'-ivfsoverlay',overlay],'android-production-compile.log',row['directory']);android={'passed':True,'object_sha256':sha(obj)}
 assert pins=={str(f.relative_to(ROOT)):sha(f)for f in paths},'Source changed during checks'
 receipt={'state':'passed','host':host,'android':android,'source_sha256':pins,'scope':'Actual production state wait;32 pthread0->ready/failed publications with acquire payload visibility and real1ms sleeps; ready/failed immediate cases do not block or accountwait. Both graphics program paths and pipeline use helper; asynchronous worker/background queues remain. No device first-use latency claim.'};(out/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
