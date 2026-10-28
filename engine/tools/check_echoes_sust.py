#!/usr/bin/env python3
"""Actual SUST translation / GLSL / SPIR-V checks; no device, GPU or game execution.
Optional --capture reads a private original shader blob; no game input is embedded.
"""
import argparse,hashlib,json,os,shutil,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/echoes-sust'
SRC=ROOT/'ports/shared-android/runtime/nvn/maxwell/translate.c';TEST=ROOT/'tests/echoes_sust_test.c'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60)
 if p.returncode:raise RuntimeError(f'{args[0]} failed:\n{p.stdout}{p.stderr}')
 return p.stdout.strip()
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--capture',type=Path);ap.add_argument('--android',action='store_true');args=ap.parse_args()
 OUT.mkdir(parents=True,exist_ok=True);cc=shutil.which('clang');glslang=shutil.which('glslangValidator');val=shutil.which('spirv-val')
 if not all([cc,glslang,val]):raise RuntimeError('Installed clang and GLSL/SPIR-V tools required; no downloads')
 results={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  runner=OUT/name;run([cc,'-std=c11','-D_GNU_SOURCE',*flags,'-I',SRC.parent,'-I',ROOT/'native',TEST,SRC,'-lm','-o',runner]);results[name]=run([runner,OUT]);print(name+': '+results[name],flush=True)
 captures={}
 if args.capture:
  helper=OUT/'capture.c';helper.write_text('''#include "shader.h"
#include <stdio.h>
#include <stdlib.h>
bool shared_title_las(void){return false;}
int main(int n,char**v){if(n!=3)return 2;FILE*f=fopen(v[1],"rb");if(!f)return 3;fseek(f,0,SEEK_END);long z=ftell(f);rewind(f);void*b=calloc(1,z);fread(b,1,z,f);fclose(f);maxwell_shader_int64=0;maxwell_gmem_mirror=1;maxwell_gmem_base=0x1240000000ull;maxwell_gmem_size=0xd0000000ull;maxwell_vote_stages=1u<<STAGE_COMPUTE;maxwell_storage_image_write=1;ShaderKey k={.glsl=1};ShaderInfo*s=maxwell_translate(b,z,STAGE_COMPUTE,&k);if(!s->msl){fprintf(stderr,"%s\\n",s->error);return 4;}f=fopen(v[2],"w");fputs(s->msl,f);fclose(f);printf("images:");for(int i=0;i<s->ntex;i++)if(s->tex[i].is_image)printf(" %d:cb%u+%x",i,s->tex[i].bindless_cbuf,s->tex[i].bindless_offset);puts("");return 0;}
''');runner=OUT/'capture';run([cc,'-std=c11','-D_GNU_SOURCE','-O2','-I',SRC.parent,'-I',ROOT/'native',helper,SRC,'-lm','-o',runner]);captures={'path':str(args.capture),'sha256':hashlib.sha256(args.capture.read_bytes()).hexdigest(),'translation':run([runner,args.capture,OUT/'captured-original.comp'])};print(captures['translation'],flush=True)
 count=0;caps=[]
 for p in sorted(OUT.glob('*.comp')):
  if p.name=='captured-original.comp' and not args.capture:continue
  spv=p.with_suffix('.spv');run([glslang,'-V','--target-env','vulkan1.3',p,'-o',spv]);run([val,'--target-env','vulkan1.3',spv]);words=struct.unpack('<'+'I'*(spv.stat().st_size//4),spv.read_bytes());i=5;write=False;storage=False
  while i<len(words):
   n,op=words[i]>>16,words[i]&65535
   if op==99:write=True # OpImageWrite, a real side effect
   if op==17 and words[i+1]==56:storage=True # StorageImageWriteWithoutFormat
   i+=n
  if not write or not storage:raise RuntimeError('Missing image store / capability '+p.name)
  count+=1
 if args.android:
  roots=[Path(os.environ.get('ANDROID_NDK_HOME','/nonexistent')),Path.home()/'android-sdk/ndk/29.0.14206865']
  candidates=[]
  for r in roots:candidates+=list(r.glob('toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang'))
  if not candidates:
   candidates=list(Path.home().glob('android-sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang'))
  if not candidates:raise RuntimeError('Installed Android NDK compiler required')
  run([candidates[0],'-std=c11','-D_GNU_SOURCE','-O2','-I',SRC.parent,'-I',ROOT/'native','-c',SRC,'-o',OUT/'translate.android.o']);results['android']='actual production translator TU compiled arm64 Android API30'
 print(f'{count} actual emitted GLSL/SPIR-V programs validated; real OpImageWrite and required capability present',flush=True)
 paths=[SRC,SRC.with_name('shader.h'),TEST,Path(__file__)];report={'schema':1,'scope':'Actual translator optimized+ASan/UBSan, conservative handle provenance and explicit unsupported modes, emitted GLSL/SPIR-V validation with OpImageWrite. Optional private original shader input. No GPU visibility/view/format/gameplay proof.','source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},'results':results,'programs':count,'capture':captures,'device_actions':False};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
