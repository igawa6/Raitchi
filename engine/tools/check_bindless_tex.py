#!/usr/bin/env python3
"""Production TEX.B translator fixtures and optional private live captures; no device."""
import argparse,hashlib,json,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/bindless-tex';SRC=ROOT/'ports/shared-android/runtime/nvn/maxwell/translate.c'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=90)
 if p.returncode:raise RuntimeError(f'{args[0]} failed:\n{p.stdout}{p.stderr}')
 return p.stdout.strip()
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--captures',type=Path);a=ap.parse_args();OUT.mkdir(parents=True,exist_ok=True)
 results={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/name;run(['clang','-std=c11','-D_GNU_SOURCE',*flags,'-I',SRC.parent,'-I',ROOT/'native',ROOT/'tests/bindless_tex_test.c',SRC,'-lm','-o',exe]);results[name]=run([exe,OUT]);print(name,results[name],flush=True)
 count=0
 for p in sorted(OUT.glob('synthetic-*.*')):
  if p.suffix not in ('.vert','.frag'):continue
  spv=p.with_suffix(p.suffix+'.spv');run(['glslangValidator','-V','--target-env','vulkan1.3',p,'-o',spv]);run(['spirv-val','--target-env','vulkan1.3',spv]);count+=1
 captures=[]
 if a.captures:
  helper=OUT/'capture.c';helper.write_text('''#include "shader.h"
#include <stdio.h>
#include <stdlib.h>
bool shared_title_las(void){return false;}
int main(int n,char**v){FILE*f=fopen(v[1],"rb");fseek(f,0,SEEK_END);long z=ftell(f);rewind(f);void*b=calloc(1,z);fread(b,1,z,f);fclose(f);ShaderKey k={.glsl=1,.vs_out_mask=~0u};maxwell_shader_int64=0;maxwell_gmem_mirror=1;maxwell_gmem_base=0x1240000000ull;maxwell_gmem_size=0xd0000000ull;maxwell_storage_image_write=1;ShaderInfo*s=maxwell_translate_sized(b,z,0,atoi(v[2]),&k);if(!s->msl){fprintf(stderr,"%s",s->error);return 4;}f=fopen(v[3],"w");fputs(s->msl,f);fclose(f);printf("%d resources",s->ntex);maxwell_free(s);free(b);return 0;}''')
  exe=OUT/'capture';run(['clang','-std=c11','-D_GNU_SOURCE','-O2','-I',SRC.parent,'-I',ROOT/'native',helper,SRC,'-lm','-o',exe])
  for i,d in enumerate(json.loads(a.captures.read_text())['matches']):
   if not d.get('private_binary'):continue
   # Derived GLSL/SPIR-V stays alongside private captures, never asset-free test fixtures.
   dest=a.captures.parent/f'candidate22-texb-{i}.{"vert" if d["stage"]==0 else "frag"}'
   result=run([exe,d['private_binary'],d['stage'],dest]);spv=dest.with_suffix(dest.suffix+'.spv');run(['glslangValidator','-V','--target-env','vulkan1.3',dest,'-o',spv]);run(['spirv-val','--target-env','vulkan1.3',spv]);opt=spv.with_suffix('.opt.spv');run(['spirv-opt','-O','--target-env=vulkan1.3',spv,'-o',opt]);run(['spirv-val','--target-env','vulkan1.3',opt]);captures.append({'index':i,'stage':d['stage'],'result':result,'input_sha256':hashlib.sha256(Path(d['private_binary']).read_bytes()).hexdigest(),'spirv_optimized_valid':True})
 report={'scope':'Actual production translator, asset-free synthetic fixtures and optional private live GLSL/SPIR-V; no GPU/gameplay proof','results':results,'synthetic_spirv':count,'captures':captures,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [SRC,SRC.with_name('shader.h'),ROOT/'tests/bindless_tex_test.c',Path(__file__)]}}
 (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(f'{count} synthetic + {len(captures)} private live GLSL/SPIR-V validated')
if __name__=='__main__':main()
