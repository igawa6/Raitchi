#!/usr/bin/env python3
"""Actual emitted shader checks for EOW HSET(P) immediate, warp32 VOTE and RED.
No GPU execution; controlled scalar execution checks the emitted reduction math.
"""
import ctypes,hashlib,json,random,re,shutil,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'ports/shared-android/runtime/nvn/maxwell/translate.c'
TEST=ROOT/'tests/echoes_shader_ops_test.c'
OUT=ROOT/'build/echoes-shader-ops'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60)
 if p.returncode:raise RuntimeError(f'{args[0]} failed:\n{p.stdout}{p.stderr}')
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True)
 cc,cxx,glslang,val=map(shutil.which,['clang','clang++','glslangValidator','spirv-val'])
 if not all([cc,cxx,glslang,val]):raise RuntimeError('Installed compilers and SPIR-V tools required')
 results={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  runner=OUT/name;run([cc,'-std=c11','-D_GNU_SOURCE',*flags,'-I',SOURCE.parent,'-I',ROOT/'native',TEST,SOURCE,'-lm','-o',runner])
  results[name]=run([runner,OUT]);print(name+': '+results[name],flush=True)
 files=sorted(p for p in OUT.iterdir() if p.suffix in ('.comp','.frag','.vert'));expr=[];atomic_bodies=[]
 for p in files:
  spv=p.with_suffix(p.suffix+'.spv');run([glslang,'-V','--target-env','vulkan1.3',p,'-o',spv]);run([val,'--target-env','vulkan1.3',spv])
  if p.name.startswith('red-0-') or p.name.startswith('actual-red'):
   words=struct.unpack('<'+'I'*(len(spv.read_bytes())//4),spv.read_bytes());i=5
   while i<len(words):
    count,op=words[i]>>16,words[i]&65535
    if op==17 and words[i+1]==11:raise RuntimeError('Unexpected Int64 capability '+p.name)
    i+=count
  if p.name.startswith('red-') and 'chunks' not in p.name:
   m=re.search(r'atomicCompSwap\(gfast_\.w\[fw\], o_, ([^;]+)\); if',p.read_text())
   if not m:raise RuntimeError('Missing emitted RED CAS expression '+p.name)
   expr.append((p.name,m[1]))
   body=re.search(r'if \(fw != 0xFFFFFFFFu\) \{ (uint v_ = .*?) \}\n      else',p.read_text(),re.S)
   if not body:raise RuntimeError('Missing actual emitted RED CAS loop '+p.name)
   atomic_bodies.append(body[1].replace('atomicCompSwap(gfast_.w[fw],','atomicCAS(cell,'))
 source='#include <cstdint>\n#include <algorithm>\n#include <thread>\n#include <vector>\nusing uint=uint32_t;using std::min;using std::max;\nstatic uint atomicCAS(uint* p,uint a,uint b){__atomic_compare_exchange_n(p,&a,b,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return a;}\n'
 for i,(_,e) in enumerate(expr):
  source+=f'extern "C" uint e{i}(uint o_,uint v_){{return {e};}}\n'
  source+=f'static void a{i}(uint*cell,uint r2){{{atomic_bodies[i]}}}\n'
  source+=f'extern "C" uint t{i}(uint initial,uint value){{uint cell=initial;std::vector<std::thread> ts;for(int n=0;n<4;n++)ts.emplace_back([&](){{for(int k=0;k<1000;k++)a{i}(&cell,value);}});for(auto&t:ts)t.join();return cell;}}\n'
 cpp=OUT/'emitted.cpp';cpp.write_text(source);libpath=OUT/'emitted.so';run([cxx,'-std=c++17','-shared','-fPIC','-O2','-pthread',cpp,'-o',libpath]);lib=ctypes.CDLL(str(libpath))
 rng=random.Random(832);values=[0,1,2,0x7fffffff,0x80000000,0xfffffffe,0xffffffff]+[rng.getrandbits(32) for _ in range(20)];checks=0
 signed=lambda v:v if v<0x80000000 else v-0x100000000
 for i,(name,_) in enumerate(expr):
  _,i64,stage,sgn,op=Path(name).stem.split('-');sgn,op=int(sgn),int(op)
  fn=getattr(lib,f'e{i}');fn.argtypes=[ctypes.c_uint32]*2;fn.restype=ctypes.c_uint32
  for o in values:
   for v in values:
    a,b=(signed(o),signed(v)) if sgn else (o,v)
    want=[lambda:o+v,lambda:min(a,b),lambda:max(a,b),lambda:0 if o>=v else o+1,lambda:v if o==0 or o>v else o-1,lambda:o&v,lambda:o|v,lambda:o^v][op]()&0xffffffff
    if fn(o,v)!=want:raise RuntimeError(f'{name}: {o:x},{v:x}')
    checks+=1
 atomic_checks=0
 for i,(name,_) in enumerate(expr):
  _,i64,stage,sgn,op=Path(name).stem.split('-');sgn,op=int(sgn),int(op)
  fn=getattr(lib,f't{i}');fn.argtypes=[ctypes.c_uint32]*2;fn.restype=ctypes.c_uint32
  for initial,value in [(0,127),(0xffffffff,0x80000005),(0x7fffffff,0x00ff00ff)]:
   want=initial;step=getattr(lib,f'e{i}')
   for _ in range(4000):want=step(want,value)
   if fn(initial,value)!=want:raise RuntimeError('Emitted CAS contention '+name)
   atomic_checks+=4000
 # Derive warp32 vote semantics for sparse active lanes and all host subgroup sizes supported by the contract.
 vote_checks=0
 for size in [32,64,96,128]:
  for _ in range(200):
   active=rng.getrandbits(size);pred=rng.getrandbits(size)&active
   for lane in range(size):
    if not active>>lane&1:continue
    word=lane>>5;mask=pred>>(word*32)&0xffffffff;am=active>>(word*32)&0xffffffff
    lane_values=[bool(pred>>j&1) for j in range(word*32,word*32+32) if active>>j&1]
    assert (mask==am,mask!=0,mask==0 or mask==am)==(all(lane_values),any(lane_values),len(set(lane_values))==1)
    vote_checks+=3
 print(f'{len(files)} actual emitted GLSL programs/SPIR-V validated; {checks} emitted RED math checks; {vote_checks} warp32 ballot comparisons; {atomic_checks} contended emitted-CAS operations',flush=True)
 paths=[SOURCE,SOURCE.with_name('shader.h'),TEST,Path(__file__)]
 report={'schema':1,'scope':'Actual translator optimized+ASan/UBSan, observed opcode fixtures and synthetic stages/capabilities; actual emitted GLSL/SPIR-V validation and scalar reduction expressions; controlled warp32 ballot model. Actual emitted CAS loops run under4 host threads with controlled host atomic built-in. Not GPU atomics, graphics mirror visibility or gameplay validation.','source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},'results':results,'programs':len(files),'emitted_red_math_checks':checks,'warp32_model_checks':vote_checks,'emitted_cas_host_atomic_operations':atomic_checks,'device_actions':False}
 (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
