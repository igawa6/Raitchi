#!/usr/bin/env python3
"""Real ImGui boot preparation controls and controlled SDL/worker lifecycle; host only."""
import argparse, hashlib, json, pathlib, shlex, shutil, subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
SOURCE=ROOT/'ports/shared-android/runtime/ui/menu.mm'
TEST=ROOT/'tests/startup_prep_ui_test.cpp'
CASES=('touch_continue','mouse_cancel','touch_cancelled','controller','keyboard','progress','stop_before','stop_during','quit','back','pad_back','quit_over_continue','fast_stop','late_stop','ready_cancel','background_stop','background_restore','hotplug','thread_failure','normal')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args()
 out=ROOT/'build/startup-prep-ui';out.mkdir(parents=True,exist_ok=True)
 s=SOURCE.read_text();start=s.index('#ifndef __APPLE__\nstatic std::atomic<int> g_prep_done');end=s.index('\nstatic TouchUi g_touch;',start)
 key=s[s.index('static ImGuiKey key_from_scancode('):s.index('// ---- software keyboard')]
 fragment=key+s[start:end]
 # Controlled wrappers record real ImGui item rectangles/progress, and thread ownership.
 for old,new in [('ImGui::Button(', 'test_button('),('ImGui::ProgressBar(', 'test_progress_bar('),('pthread_create(', 'test_pthread_create('),('pthread_join(', 'test_pthread_join(')]:fragment=fragment.replace(old,new)
 (out/'startup_prep_ui.inc').write_text(fragment)
 assert 'ImGuiWindowFlags_NoInputs' not in fragment
 assert 'rt_reboot_requested()' in fragment and 'return action != PREP_QUIT;' in fragment
 main=(ROOT/'ports/shared-android/runtime/main.c').read_text()
 gate=main.index('bool preparation_continue = menu_shader_prep_startup();')
 assert main.index('vk_shader_preparation_mode(true);')<gate
 assert gate<main.index('vk_shader_preparation_mode(false);')<main.index('if (!preparation_continue)')<main.index('vk_headstart_finish();')<main.index('kernel_create_main_thread(')
 imgui=ROOT/'work/shared-android/third_party/imgui';sdl=ROOT/'work/las-baseline/build/android/SDL3/include'
 core=[imgui/x for x in ('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp')]
 paths=[SOURCE,TEST,pathlib.Path(__file__).resolve(),*core];pins={str(x.relative_to(ROOT)):sha(x) for x in paths}
 def run(cmd,name,cwd=None):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=120,cwd=cwd);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang++'),'-std=c++17',*flags,'-I',imgui,'-I',sdl,'-I',out,TEST,*core,'-pthread','-o',exe],name+'-compile.log')
  reports[name]={case:json.loads(run([exe,case],name+'-'+case+'.log'))for case in CASES}
  print(name+': '+str(len(CASES))+' scenarios passed')
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(x for x in rows if x['file'].endswith('/runtime/ui/menu.mm'));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(out/'menu-android.o');overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':row['file'],'external-contents':str(SOURCE)}]}));run(cmd+['-ivfsoverlay',overlay],'android-compile.log',row['directory']);android={'passed':True,'object_sha256':sha(out/'menu-android.o')}
 assert pins=={x:sha(ROOT/x)for x in pins},'Source changed during checks'
 (out/'verification.json').write_text(json.dumps({'state':'passed','source_sha256':pins,'variants':reports,'android':android,'scope':'Actual preparation gate/input/draw region, real ImGui CPU controls, controlled SDL events/clock and headstart callback; no GPU/device or cache codec validation.'},indent=2)+'\n')
if __name__=='__main__':main()
