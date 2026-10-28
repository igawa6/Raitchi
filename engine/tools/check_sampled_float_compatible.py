#!/usr/bin/env python3
"""Extract actual numeric format policy with exact production structs; no device."""
import hashlib,json,shlex,subprocess
from pathlib import Path
from check_storage_image_views import function,struct
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/sampled-float-compatible'
def run(cmd):
 p=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=90)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);src=ROOT/'ports/shared-android/runtime/nvn/vulkan/textures.c';hdr=src.with_name('vk.h');nvn=src.parent.parent/'nvn.h';nt=nvn.read_text();text=src.read_text()
 types='typedef void *VmaAllocation;typedef struct {unsigned unused;} TexLayout;typedef struct NvnPool NvnPool;\n'+struct(nt,'typedef struct NvnTexture {','} NvnTexture;')+'\n'
 start=nt.rfind('typedef struct {',0,nt.index('} NvnTextureView;'));types+=nt[start:nt.index('} NvnTextureView;',start)+len('} NvnTextureView;')]+'\n'+struct(hdr.read_text(),'typedef struct TexBackend {','} TexBackend;')+'\n';(OUT/'production_types.inc').write_text(types)
 names=['fmt_of','renderer_format_is_depth','renderer_format_has_stencil','vk_storage_format_kind','vk_color_bytes','vk_block_class','vk_view_compatible','vk_sampled_stencil','sampled_nvn_format_known','vk_sampled_float_compatible']
 (OUT/'production_helpers.inc').write_text('typedef struct {VkFormat vf;uint8_t expand_rgb,alpha_one;} VkFmt;\n'+'\n'.join(function(text,n)for n in names)+'\n')
 results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run(['clang','-std=c11','-D_GNU_SOURCE','-Wno-missing-field-initializers',*flags,'-I',OUT,'-I',ROOT/'work/las-baseline/build/android/deps/include',ROOT/'tests/sampled_float_compatible_test.c','-o',exe]);results[mode]=run([exe]);print(results[mode],flush=True)
 # Compile the whole production Android texture TU, preserving all prior mappings.
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/textures.c'))
 overlay=OUT/'overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':row['file'],'external-contents':str(src)},{'type':'file','name':str(Path(row['file']).with_name('vk.h')),'external-contents':str(hdr)}]}));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(OUT/'textures-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd)
 paths=[src,hdr,ROOT/'tests/sampled_float_compatible_test.c',Path(__file__)];report={'scope':'Actual production format/classifier helpers; exhaustive supported NVN map plus effective views and actual backend formats. No GPU/device qualification.','tests':results,'android_full_tu':True,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print('Full Android texture TU compiled')
if __name__=='__main__':main()
