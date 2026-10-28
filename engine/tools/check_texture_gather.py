#!/usr/bin/env python3
"""Production bound2D TLD4/TLD4S fixtures, GLSL and real gather SPIR-V."""
import hashlib,json,pathlib,shutil,struct,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];SRC=ROOT/'ports/shared-android/runtime/nvn/maxwell/translate.c';OUT=ROOT/'build/texture-gather'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=90)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);paths=[SRC,SRC.with_name('shader.h'),ROOT/'tests/texture_gather_test.c',pathlib.Path(__file__)];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths};results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run([shutil.which('clang'),'-std=c11','-D_GNU_SOURCE',*flags,'-I',SRC.parent,'-I',ROOT/'native',ROOT/'tests/texture_gather_test.c',SRC,'-lm','-o',exe]);results[mode]=run([exe,OUT]);print(mode,results[mode],flush=True)
 count=0
 for p in sorted(OUT.glob('gather-*.*')):
  if p.suffix not in('.vert','.frag'):continue
  spv=p.with_suffix(p.suffix+'.spv');run(['glslangValidator','-V','--target-env','vulkan1.3',p,'-o',spv]);run(['spirv-val','--target-env','vulkan1.3',spv]);words=struct.unpack('<'+'I'*(spv.stat().st_size//4),spv.read_bytes());i=5;gather=False
  while i<len(words):
   n,op=words[i]>>16,words[i]&65535
   if op==96:gather=True # OpImageGather
   i+=n
  if not gather:raise RuntimeError('No actual OpImageGather: '+p.name)
  count+=1
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}
 report={'state':'passed','results':results,'spirv_gathers':count,'source_sha256':pins,'scope':'Asset-free generic ISA fields; production translator and actual OpImageGather. Bound2D F32TLD4S/fullTLD4, component0..3, quad masks, float/UINT binding, destination/coordinate overlap, RZ and unsupported forms. No offset, depth compare, sparse-result, F16S, bindless-gather or GPU-image correctness claim.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
