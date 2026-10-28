#!/usr/bin/env python3
"""Actual private source-export helpers + strict selector + production kernel copy."""
import hashlib,json,re,shlex,subprocess
from pathlib import Path
from check_storage_image_views import function,struct
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/material-projection';RT=ROOT/'ports/shared-android/runtime'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=90)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);src=RT/'nvn/vulkan/exec.c';text=src.read_text();nt=(RT/'nvn/nvn.h').read_text();fp=(ROOT/'work/shared-android/runtime/nvn/shader_prep.h').read_text();prep=(ROOT/'work/shared-android/runtime/nvn/shader_prep.c').read_text()
 types=struct(nt,'typedef struct NvnShaderStage {','} NvnShaderStage;')+'\n'+struct(fp,'typedef struct ShaderFp {','} ShaderFp;')+'\ntypedef struct {uint64_t addr,size;}Range;\ntypedef struct {ShaderFp fp[2];ShaderInfo *vi,*fi;}Prog;\n';(OUT/'production_projection_types.inc').write_text(types)
 decl=text[text.index('#define MATERIAL_PROJECTION_MAX_BYTES'):text.index('static bool material_projection_range(')];(OUT/'production_projection.inc').write_text(decl+'\n'+function(prep,'shader_code_hash')+'\n'+'\n'.join(function(text,n)for n in ['material_projection_range','material_projection_payload','material_projection_shader','material_projection_uniforms'])+'\n')
 kh=(RT/'kernel.h').read_text();limit=re.search(r'(?m)^#define KERNEL_READABLE_COPY_MAX[^\n]+',kh)[0];(OUT/'production_kernel_copy.inc').write_text(limit+'\n'+function((RT/'kernel.c').read_text(),'kernel_copy_readable')+'\n')
 results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run(['clang','-std=c11',*flags,'-I',OUT,'-I',ROOT/'native','-I',src.parent.parent/'maxwell',ROOT/'tests/material_projection_test.c','-pthread','-o',exe]);results[mode]=run([exe]);print(mode,results[mode],flush=True)
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/exec.c'));overlay=OUT/'overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':row['file'],'external-contents':str(src)},{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/tegra.h'),'external-contents':str(ROOT/'ports/shared-android/runtime/nvn/tegra.h')}]}));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(OUT/'exec-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd)
 paths=[src,RT/'kernel.c',RT/'kernel.h',ROOT/'tests/material_projection_test.c',Path(__file__)];(OUT/'verification.json').write_text(json.dumps({'scope':'Actual strictselector/sourcepayload/shaderidentity helpers and real kernel_copy_readable against controlled readable-page inventory+guardpage;2MiBaggregate budget,overflows,constantspan/fullstagehash. No ownedassets/device/GPUcache readback.','results':results,'android_full_tu':True,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}},indent=2)+'\n');print('AndroidexecTU compiled')
if __name__=='__main__':main()
