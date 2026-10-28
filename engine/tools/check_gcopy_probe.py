#!/usr/bin/env python3
"""Actual GCOPY hit diagnostic/cache behavior, safety bounds and Android compile."""
import argparse,hashlib,json,pathlib,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args();out=ROOT/'build/gcopy-probe';out.mkdir(parents=True,exist_ok=True)
 runtime=ROOT/'ports/shared-android/runtime';source=runtime/'nvn/vulkan/exec.c';test=ROOT/'tests/gcopy_probe_test.c';paths=[source,test,pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x)for x in paths}
 text=source.read_text();start=text.index('#define GCOPY_TAB ');end=text.index('// ================================================================== worker threads',start);fragment=text[start:end];(out/'gcopy_probe.production.h').write_text(fragment)
 assert 'gcopy_probe_hit(addr, size, gcopy[slot].span);'in fragment
 def run(cmd,name,cwd=None):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,cwd=cwd,timeout=90);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,test,'-o',exe],name+'-build.log');reports[name]={mode:json.loads(run([exe,mode],name+'-'+mode+'-run.log'))for mode in('1','0','disabled')};reports[name]['small_bypass']=json.loads(run([exe,'1','1'],name+'-small-bypass.log'))
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/runtime/nvn/vulkan/exec.c'));overlay=out/'overlay.json';owned=[source,runtime/'nvn/vulkan/vk.h',runtime/'nvn/maxwell/shader.h'];overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/f.relative_to(runtime)),'external-contents':str(f)}for f in owned]}));cmd=shlex.split(row['command']);obj=out/'exec-android.o';cmd[cmd.index('-o')+1]=str(obj);run([*cmd,'-ivfsoverlay',overlay],'android-production-compile.log',row['directory']);android={'passed':True,'object_sha256':sha(obj)}
 assert pins=={str(x.relative_to(ROOT)):sha(x)for x in paths},'Source changed during checks'
 receipt={'state':'passed','host_tests':reports,'android_production_compile':android,'source_sha256':pins,'scope':'Actual production cache and diagnostic bodies; small-range mutation/dedup and large bootstrap starvation tests. Range validation includes zero, overflow, out-of-window and >8192B rejection;64unique mismatch cap. Default cache policy unchanged. Optional GCOPY_BYPASS_SMALL diagnostic refreshes only<=8192B ranges; larger copies retain cache. No device/game evidence.'};(out/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
