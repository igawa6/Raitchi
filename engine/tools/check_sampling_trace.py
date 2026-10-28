#!/usr/bin/env python3
"""Production default-off bounded trace with exact structs and full Android TU."""
import hashlib,json,shlex,subprocess
from pathlib import Path
from check_storage_image_views import function,struct
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/sampling-trace'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=90)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);src=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c';nvn=src.parent.parent/'nvn.h';hdr=src.with_name('vk.h');nt=nvn.read_text();text=src.read_text()
 types='typedef void *VmaAllocation;typedef struct {unsigned unused;} TexLayout;typedef struct NvnPool NvnPool;\n'+struct(nt,'typedef struct NvnTexture {','} NvnTexture;')+'\n'
 start=nt.rfind('typedef struct {',0,nt.index('} NvnTextureView;'));types+=nt[start:nt.index('} NvnTextureView;')+len('} NvnTextureView;')]+'\n';start=nt.rfind('typedef struct {',0,nt.index('} NvnSamplerDesc;'));types+=nt[start:nt.index('} NvnSamplerDesc;')+len('} NvnSamplerDesc;')]+'\n'
 start=nt.rfind('typedef struct {',0,nt.index('} NvnTexEntry;'));types+=nt[start:nt.index('} NvnTexEntry;')+len('} NvnTexEntry;')]+'\n'+struct(hdr.read_text(),'typedef struct TexBackend {','} TexBackend;')+'\n';(OUT/'production_types.inc').write_text(types)
 (OUT/'production_sampling_trace.inc').write_text(text[text.index('static int sampling_trace_enabled ='):text.index('static void sampling_trace_binding(')]+function(text,'sampling_trace_binding')+'\n'+function(text,'material_sampler_metadata')+'\n')
 with (OUT/'production_sampling_trace.inc').open('a') as f:f.write(text[text.index('static uint64_t material_projection_fs ='):text.index('typedef struct {\n    FILE *meta, *data;')])
 (OUT/'production_material_gate.inc').write_text(text[text.index('static int material_probe_enabled ='):text.rfind('static void material_probe_view(FILE *f,')])
 assert 'sampling_trace_poll();' in text
 assert 'sampled_texture_handle(1, image, &handle)' in function(text,'material_projection_samplers')
 assert 'sampled_texture_handle(1, image, &handle)' in function(text[text.rfind('static void material_probe_draw('):],'material_probe_draw')
 assert 'material_sampler_metadata(f, i, tsc, sampler, known);' in function(text[text.rfind('static void material_probe_draw('):],'material_probe_draw')
 results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run(['clang','-std=c11',*flags,'-I',OUT,'-I',src.parent.parent/'maxwell','-I',ROOT/'work/las-baseline/build/android/deps/include',ROOT/'tests/sampling_trace_test.c','-o',exe]);results[mode]={str(g):run([exe,g])for g in [0,1,2,'unset']};print(mode,results[mode],flush=True)
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/exec.c'));overlay=OUT/'overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':row['file'],'external-contents':str(src)},{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/tegra.h'),'external-contents':str(ROOT/'ports/shared-android/runtime/nvn/tegra.h')}]}));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(OUT/'exec-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd)
 paths=[src,ROOT/'tests/sampling_trace_test.c',Path(__file__)];(OUT/'verification.json').write_text(json.dumps({'results':results,'android_full_tu':True,'scope':'Verbatim production trace gate/formatter, exacthoststructs, disabledguardpage/noformatlookups,64unique cap, dedupkey fields,pool/defaultmetadata. No Vulkan/device execution.','source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}},indent=2)+'\n');print('Android full exec TU compiled')
if __name__=='__main__':main()
