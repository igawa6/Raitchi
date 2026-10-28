#!/usr/bin/env python3
"""Actual adapter selection/accessor and texture policy; no device/guest execution."""
import hashlib,json,shlex,subprocess
from pathlib import Path
from check_storage_image_views import function
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/renderer-policy';RT=ROOT/'ports/shared-android/runtime'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=90)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);tex=RT/'nvn/vulkan/textures.c';gam=RT/'gamelib.c';text=tex.read_text()
 (OUT/'production_renderer_policy.inc').write_text(function(gam.read_text(),'shared_renderer_legacy_target_policy')+'\n'+function(text,'renderer_format_is_depth')+'\n'+function(text[text.rfind('static float shadow_scale('):],'shadow_scale')+'\n'+'\n'.join(function(text,n)for n in ['tex_scalable','vk_level_change_check'])+'\n')
 sources=[ROOT/p for p in ['native/module_abi.c','native/runtime/native_manifest.c','native/runtime/session_layout.c','native/adapters/title_adapter.c','native/adapters/las/profile.c','native/adapters/letsgo/profile.c','native/adapters/echoes/profile.c','native/adapters/wonder/profile.c']];results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run(['clang','-std=c11',*flags,'-I',OUT,'-I',ROOT/'native','-I',RT,'-I',ROOT/'work/shared-android/runtime/nvn',ROOT/'tests/renderer_policy_test.c',*sources,'-o',exe]);results[mode]={str(i):run([exe,i])for i in range(1,5)};print(mode,results[mode],flush=True)
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());compiled=[]
 for suffix,source in [('nvn/vulkan/textures.c',tex),('gamelib.c',gam)]:
  row=next(r for r in rows if r['file'].endswith('/runtime/'+suffix));overlay=OUT/(source.stem+'-overlay.json');virtual=Path(row['file']);workrt=virtual.parents[2]if suffix.startswith('nvn/')else virtual.parent
  overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':row['file'],'external-contents':str(source)},{'type':'file','name':str(workrt/'common_session.h'),'external-contents':str(RT/'common_session.h')},{'type':'file','name':str(workrt.parent/'native/adapters/title_adapter.h'),'external-contents':str(ROOT/'native/adapters/title_adapter.h')}]}));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(OUT/(source.stem+'-android.o'));cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd);compiled.append(suffix)
 paths=sources+[tex,gam,RT/'common_session.h',ROOT/'native/adapters/title_adapter.h',ROOT/'tests/renderer_policy_test.c',Path(__file__)];(OUT/'verification.json').write_text(json.dumps({'results':results,'android_units':compiled,'scope':'Actual immutable adapter flags/selection and extracted bridge/texture policies. Artifact caps/manifests/CPU contracts unchanged; GPU behavior independently device-qualified.','source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}},indent=2)+'\n');print('Androidtexture+gamelibTUs compiled')
if __name__=='__main__':main()
