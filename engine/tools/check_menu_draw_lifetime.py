#!/usr/bin/env python3
"""Actual ImGui CPU draw lifetime and menu source integration regression."""
import argparse,hashlib,json,pathlib,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args();out=ROOT/'build/menu-draw-lifetime';out.mkdir(parents=True,exist_ok=True);source=ROOT/'ports/shared-android/runtime/ui/menu.mm';test=ROOT/'tests/menu_draw_lifetime_test.cpp';imgui=ROOT/'work/shared-android/third_party/imgui';s=source.read_text();body=s[s.index('    bool open = g_open.load();',s.index('void menu_render(')):s.index('// The last menu_render')]
 assert body.index('ImGui::NewFrame();')<body.index('ImGui::Render();')<body.index('ImGui_ImplVulkan_RenderDrawData(')
 assert body.count('ImGui_ImplVulkan_RenderDrawData(')==1 and 'g_ov_same' not in body and 'ImDrawData *dd' not in body
 assert 'if (g_ov_cache) fps_note(now, io.DeltaTime);' in body
 paths=[source,test,pathlib.Path(__file__).resolve()]+[imgui/x for x in ('imgui.h','imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp')];pins={str(x.relative_to(ROOT)):sha(x) for x in paths}
 def run(args,name,cwd=None):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=120,cwd=cwd);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang++'),'-std=c++17',*flags,'-I',imgui,test,*[imgui/x for x in ('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp')],'-o',exe],name+'-build.log');reports[name]=json.loads(run([exe],name+'-run.log'))
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(x for x in rows if x['file'].endswith('/runtime/ui/menu.mm'));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(out/'menu-android.o');overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime/ui/menu.mm'),'external-contents':str(source)}]}));run([*cmd,'-ivfsoverlay',overlay],'android-production-compile.log',row['directory']);android={'passed':True,'object_sha256':sha(out/'menu-android.o')}
 assert pins=={k:sha(ROOT/k) for k in pins},'Source changed during checks'
 report={'state':'passed','host_tests':reports,'android_production_compile':android,'source_sha256':pins,'scope':'Actual ImGui core CPU vertex/index uploads over900freshframes, idle/input/menu transitions and garbagecollection; menu integration ordering/sourceguard. No Vulkan memory-map or device validation.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
