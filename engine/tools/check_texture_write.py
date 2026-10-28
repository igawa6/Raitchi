#!/usr/bin/env python3
"""Actual partial texture uploads versus existing full GOB swizzle and API wrappers."""
import hashlib,json,pathlib,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];OUT=ROOT/'build/texture-write';RUNTIME=ROOT/'ports/shared-android/runtime';NVN=RUNTIME/'nvn'
def function(s,signature):
 start=s.index(signature);a=s.index('{',start);depth=1;i=a+1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]+'\n'
def main():
 OUT.mkdir(parents=True,exist_ok=True);assert 'case 0x98: return (VkFmt){VK_FORMAT_A1R5G5B5_UNORM_PACK16};' in (NVN/'vulkan/textures.c').read_text();s=(NVN/'objects.c').read_text();start=s.index('// NVNcopyRegion ABI:');end=s.index('NVN(nvnTextureReadTexels)',start);(OUT/'texture_write_bodies.h').write_text(function(s,'bool nvn_pool_resolve_span(')+s[start:end]);v=(NVN/'vulkan/textures.c').read_text();(OUT/'texture_coherence_bodies.h').write_text(v[v.index('// BEGIN explicit CPU texture-write coherence'):v.index('// END explicit CPU texture-write coherence')]);e=(NVN/'vulkan/exec.c').read_text();assert e.index('VkImageView v = vk_view(t, &st.rt.cview[i]')<e.index('renderer_mark_gpu_owned(t); // vk_view');assert 'if (watch && !always_full && !written)' in v and 'bool full = written ||' in v and 'if (b->gpu_owned) texture_write_gpu_check(t, b);' in v;paths=[NVN/f for f in ['objects.c','pool_mapping.c','pool_mapping.h','tegra.c','tegra.h','formats.c','nvn.h','vulkan/textures.c','vulkan/vk.h','vulkan/exec.c']]+[ROOT/'tests/texture_write_test.c',ROOT/'tests/texture_coherence_test.c',pathlib.Path(__file__).resolve()];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
 def run(command,log,cwd=None):
  p=subprocess.run(list(map(str,command)),cwd=cwd,capture_output=True,text=True,timeout=90);(OUT/log).write_text(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(log+': '+p.stdout+p.stderr)
  return p.stdout.strip()
 common=['-std=gnu11','-Wall','-Wextra','-Werror','-I',OUT,'-I',ROOT/'native','-I',RUNTIME,'-I',ROOT/'work/shared-android/runtime/nvn',ROOT/'tests/texture_write_test.c',NVN/'tegra.c',NVN/'formats.c',NVN/'pool_mapping.c','-pthread'];result={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/name;run([shutil.which('clang'),*common,*flags,'-o',exe],name+'-build.log');result[name]=run([exe],name+'-run.log');coherence=OUT/(name+'-coherence');coherence_command=[x for x in common if x not in [ROOT/'tests/texture_write_test.c',NVN/'formats.c']];run([shutil.which('clang'),*coherence_command,*flags,ROOT/'tests/texture_coherence_test.c','-o',coherence],name+'-coherence-build.log');result[name+'-coherence']=run([coherence],name+'-coherence-run.log')
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());compiled=[]
 overlay=OUT/'canonical-overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/p.relative_to(RUNTIME)),'external-contents':str(p)}for p in paths if p.is_relative_to(RUNTIME)]}))
 for suffix in ['/runtime/nvn/objects.c','/runtime/nvn/tegra.c','/runtime/nvn/formats.c','/runtime/nvn/vulkan/textures.c','/runtime/nvn/vulkan/exec.c']:
  row=next(x for x in rows if x['file'].endswith(suffix));command=shlex.split(row['command']);command[1:1]=['-ivfsoverlay',str(overlay)];command[command.index('-o')+1]=str(OUT/(pathlib.Path(suffix).stem+'.android.o'));run(command,pathlib.Path(suffix).stem+'-android-build.log',row['directory']);compiled.append(suffix)
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},'Source changed during checks'
 report={'state':'passed','host':result,'android_production_units':compiled,'source_sha256':pins,'scope':'Actual known format geometries, GOB helpers and extracted NVN entry points; full-swizzle reference checks preserve unrelated mip/layer/depth/row/padding bytes, compressed edge updates, linear pitch and signed source strides. Explicit CPU generations bypass real uploader guards; ordered initial GPU ownership uses a controlled upload double and late GPU-owned CPU patches reject. No driver/device/rendering qualification.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
