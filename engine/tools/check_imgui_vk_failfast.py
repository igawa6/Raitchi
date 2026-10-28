#!/usr/bin/env python3
"""Actual ImGui Vulkan error callback and pre-pipeline log integration checks."""
import argparse,hashlib,json,pathlib,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args();out=ROOT/'build/imgui-vk-failfast';out.mkdir(parents=True,exist_ok=True);source=ROOT/'ports/shared-android/runtime/nvn/vulkan/imgui_glue.cpp';test=ROOT/'tests/imgui_vk_failfast_test.cpp';s=source.read_text();fragment=s[s.index('static void imgui_vk_result('):s.index('extern "C" bool vk_imgui_backend_init(')];(out/'imgui_vk_failfast_fragment.h').write_text(fragment)
 assert s.index('ii.CheckVkResultFn = imgui_vk_result;')<s.index('ImGui_ImplVulkan_Init(&ii)')
 exec_source=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c';e=exec_source.read_text();begin=e.index('LOG("[build] begin pipeline key');assert e.index('getenv("LARECOMP_LOG_PIPE_BUILD")',begin-100)<begin<e.index('VkResult r = pipe_create_graphics(',begin)
 paths=[source,exec_source,test,pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x) for x in paths}
 def run(args,name,cwd=None):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60,cwd=cwd);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang++'),'-std=c++17','-include','initializer_list',*flags,'-I',out,test,'-o',exe],name+'-build.log');reports[name]=json.loads(run([exe],name+'-run.log'))
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/vulkan'/x.name),'external-contents':str(x)}for x in (source,exec_source)]}))
  for path in (source,exec_source):
   row=next(x for x in rows if x['file'].endswith('/runtime/nvn/vulkan/'+path.name));cmd=shlex.split(row['command']);obj=out/(path.stem+'-android.o');cmd[cmd.index('-o')+1]=str(obj);run([*cmd,'-ivfsoverlay',overlay],path.stem+'-android-build.log',row['directory']);android[path.name]={'passed':True,'object_sha256':sha(obj)}
 assert pins=={k:sha(ROOT/k) for k in pins},'Owned sources changed during checks'
 report={'state':'passed','host_tests':reports,'android_production_compile':android,'source_sha256':pins,'scope':'Actual callback fail-before-copy semantics and source integration. No Vulkan allocation, device or root OOM cause claim.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
