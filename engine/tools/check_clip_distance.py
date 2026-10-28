#!/usr/bin/env python3
"""Actual clip-distance output/feature wiring, SPIR-V and clipping arithmetic checks."""
import argparse,ctypes,hashlib,json,math,pathlib,random,re,shlex,shutil,struct,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');p.add_argument('--producer',type=pathlib.Path);a=p.parse_args()
 out=ROOT/'build/clip-distance';out.mkdir(parents=True,exist_ok=True);runtime=ROOT/'ports/shared-android/runtime';source=runtime/'nvn/maxwell/translate.c';header=source.with_name('shader.h');device=runtime/'nvn/vulkan/device.c';test=ROOT/'tests/clip_distance_test.c';paths=[source,header,device,test,pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x)for x in paths}
 def run(args,name,cwd=None,fail=False):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,cwd=cwd,timeout=90);(out/name).write_text(r.stdout+r.stderr)
  if(r.returncode==0)==fail:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 cc=shutil.which('clang');reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([cc,'-std=c11','-D_GNU_SOURCE',*flags,'-I',ROOT/'native','-I',source.parent,test,source,'-lm','-o',exe],name+'-build.log');reports[name]=json.loads(run([exe,out,*([a.producer]if a.producer else[])],name+'-run.log'))
 stages=list(out.glob('*.vert'))
 for f in stages:
  spv=f.with_suffix('.spv');run([shutil.which('glslangValidator'),'-V','--target-env','vulkan1.3',f,'-o',spv],f.stem+'-spirv-build.log');run([shutil.which('spirv-val'),'--target-env','vulkan1.3',spv],f.stem+'-spirv-val.log');dis=run([shutil.which('spirv-dis'),spv],f.stem+'-spirv-dis.log');assert 'OpCapability ClipDistance'in dis and 'BuiltIn ClipDistance'in dis
 # Run the actual emitted bit reinterpretation, including subnormal/negative-zero
 # values: no coordinate/depth-range transform belongs in a user clip distance.
 emitted=(out/('actual-stencil-producer.vert'if a.producer else'mask-01.vert')).read_text();expr=re.search(r'gl_ClipDistance\[0\] = (as_f\(r\d+\));',emitted).group(1);expr=re.sub(r'r\d+','r',expr)
 c='#include <stdint.h>\n#include <string.h>\nstatic float as_f(uint32_t x){float f;memcpy(&f,&x,4);return f;}\nfloat emitted(uint32_t r){return '+expr+';}\n';(out/'clip-expression.c').write_text(c);run([cc,'-std=c11','-O2','-shared','-fPIC',out/'clip-expression.c','-o',out/'clip-expression.so'],'expression-build.log');lib=ctypes.CDLL(str(out/'clip-expression.so'));lib.emitted.argtypes=[ctypes.c_uint32];lib.emitted.restype=ctypes.c_float
 rng=random.Random(1977);numeric=0
 for bits in [0,0x80000000,1,0x80000001,0x7f7fffff,0xff7fffff,0x3f800000,0xbf800000]+[rng.getrandbits(32)for _ in range(20000)]:
  value=struct.unpack('<f',struct.pack('<I',bits))[0]
  if not math.isfinite(value):continue
  actual=lib.emitted(bits);assert struct.pack('<f',actual)==struct.pack('<f',value);assert(actual<0)==(value<0);numeric+=1
 # Triangle half-space clipping from the emitted signed distances. A right
 # triangle with distances {-1,1,1} retains three quarters of its area.
 poly=[(0.,0.,lib.emitted(0xbf800000)),(2.,0.,lib.emitted(0x3f800000)),(0.,2.,lib.emitted(0x3f800000))];clipped=[]
 for x,y in zip(poly,poly[1:]+poly[:1]):
  if x[2]>=0:clipped.append(x)
  if(x[2]>=0)!=(y[2]>=0):
   t=x[2]/(x[2]-y[2]);clipped.append(tuple(x[i]+t*(y[i]-x[i])for i in range(3)))
 area=abs(sum(x[0]*y[1]-y[0]*x[1]for x,y in zip(clipped,clipped[1:]+clipped[:1])))/2;assert area==1.5
 text=device.read_text();assert 'b->shaderClipDistance = h->shaderClipDistance;'in text and'maxwell_clip_distance = b->shaderClipDistance != VK_FALSE;'in text
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/f.relative_to(runtime)),'external-contents':str(f)}for f in(source,header,device)]}))
  for f in(source,device):
   row=next(r for r in rows if r['file'].endswith('/runtime/'+str(f.relative_to(runtime))));cmd=shlex.split(row['command']);obj=out/(f.stem+'-android.o');cmd[cmd.index('-o')+1]=str(obj);run([*cmd,'-ivfsoverlay',overlay],f.stem+'-android-build.log',row['directory']);android[f.name]={'passed':True,'object_sha256':sha(obj)}
 assert pins=={str(x.relative_to(ROOT)):sha(x)for x in paths},'Source changed during checks'
 receipt={'state':'passed','host_tests':reports,'spirv_variants':len(stages),'emitted_clip_numeric_cases':numeric,'clipped_triangle_area':area,'producer_sha256':sha(a.producer)if a.producer else None,'producer_brx_mode':'existing generic fallback; optimizer disabled only in host fixture because its scan exceeds captured declared constants','android_production_compile':android,'source_sha256':pins,'scope':'Actual translator/header mask and enabled feature wiring; synthetic all8planes/all256masks; optional captured producer translation. Real SPIR-V validation and emitted expression/half-space arithmetic. Not Vulkan GPU clipping or game visual proof.'};(out/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
