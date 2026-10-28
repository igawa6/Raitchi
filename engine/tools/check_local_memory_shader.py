#!/usr/bin/env python3
"""Execute production local-memory emission against a separate uint32 oracle; synthetic programs only."""
import argparse,ctypes,hashlib,json,os,re,subprocess,random,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/'ports/shared-android/runtime/nvn/maxwell/translate.c';TEST=ROOT/'tests/local_memory_shader_test.c'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--baseline-dir',type=Path);a=ap.parse_args()
 out=ROOT/'build/local-memory';out.mkdir(parents=True,exist_ok=True)
 def run(cmd,name,env=None):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=90,env=env);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 variants=[('optimized',SRC,SRC.parent,['-O2']),('sanitized',SRC,SRC.parent,['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]
 if a.baseline_dir:variants.append(('baseline',a.baseline_dir/'translate.c',a.baseline_dir,['-O2']))
 bodies=[];rows=None;validated=0;unchanged=0
 for variant,src,inc,flags in variants:
  exe=out/(variant+'-test');run(['clang','-std=gnu11','-I',ROOT/'native','-I',inc,'-I',SRC.parent,*flags,TEST,src,'-lm','-o',exe],variant+'-build.log')
  for mode in ('nested','pc'):
   env={k:v for k,v in os.environ.items()if not k.startswith('LARECOMP_')}
   if mode=='pc':env.update(LARECOMP_VK_NESTED_FLOW='0',LARECOMP_SHADER_PC_MACHINE='1')
   d=out/(variant+'-'+mode);d.mkdir(exist_ok=True);run([exe,d],variant+'-'+mode+'-run.log',env)
   codes=[list(map(lambda x:int(x,16),line.split()[1:]))for line in (d/'index.txt').read_text().splitlines()]
   if rows is None:rows=codes
   assert rows==codes
   for i,code in enumerate(codes):
    p=d/f'{i:02d}.frag';text=p.read_text()
    if variant!='baseline':
     sparse=i not in (8,9,10)
     assert ('uint lmem[256]'not in text)==sparse,(i,mode)
     if i==0:assert len(re.findall(r'uint lm\d+ = 0u;',text))==4
    if variant=='optimized':
     run(['glslangValidator','-V','--target-env','vulkan1.2',p,'-o',p.with_suffix('.spv')],d.name+f'-{i}-spv.log');run(['spirv-val','--target-env','vulkan1.2',p.with_suffix('.spv')],d.name+f'-{i}-val.log');validated+=1
    if variant=='baseline' and i in (8,9,10):assert text==(out/('optimized-'+mode)/p.name).read_text();unchanged+=1
    if variant=='sanitized':continue
    body=re.search(r'void main\(\)\s*\{(.*)\}\s*$',text,re.S)[1]
    body=re.sub(r'uint r(\d+) = 0u;',lambda m:f'uint r{m[1]} = input[{m[1]}];',body)
    body=re.sub(r'bool p(\d+) = false;',lambda m:f'bool p{m[1]} = (pred >> {m[1]}) & 1u;',body)
    used=sorted(set(map(int,re.findall(r'uint r(\d+) = input',body))))
    bodies.append((variant,mode,i,body,'\n'.join(f'input[{r}]=r{r};'for r in used)))
 cpp='''#include <cstdint>\n#include <cstring>\nusing uint=uint32_t;\nstatic int sx8(uint x){return int(int8_t(x));}\nstatic int sx16(uint x){return int(int16_t(x));}\nstatic uint add32(uint a,uint b,uint c,uint&carry){uint64_t s=uint64_t(a)+b+c;carry=uint(s>>32);return uint(s);}\nextern "C" void evaluate(unsigned which,uint*input,uint pred){switch(which){\n'''
 cpp+=''.join(f'case {n}:{{{body}\n{write}\nreturn;}}\n'for n,(_,_,_,body,write)in enumerate(bodies))+'default:return;}}'
 (out/'emitted.cpp').write_text(cpp);run(['clang++','-std=c++17','-O2','-shared','-fPIC',out/'emitted.cpp','-o',out/'emitted.so'],'emitted-build.log')
 lib=ctypes.CDLL(str(out/'emitted.so'));lib.evaluate.argtypes=[ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32),ctypes.c_uint32]
 mask=0xffffffff
 def reference(words,initial,pred):
  reg=list(initial);mem=[0]*256;ps=[bool(pred>>i&1)for i in range(7)]+[True];pc=1
  def read(r):return 0 if r==255 else reg[r]
  for step in range(20000):
   if pc>=len(words):return reg
   if pc%4==0:pc+=1;continue
   w=words[pc];pc+=1;pr=(w>>16)&7;enabled=ps[pr]^bool((w>>19)&1)
   if not enabled:continue
   op=w&0xfff8000000000000;d=w&255;base=(w>>8)&255
   if op in (0xef40000000000000,0xef50000000000000):
    size=(w>>48)&7;off=(w>>20)&0xffffff;off=off-(1<<24)if off&(1<<23)else off
    address=(read(base)+off)&mask;slot=(address>>2)&255;shift=(address&3)*8;n=2 if size==5 else 4 if size>=6 else 1
    if op==0xef50000000000000:
     if size<4:
      m=255 if size<2 else 65535;mem[slot]=((mem[slot]&~((m<<shift)&mask))|((read(d)&m)<<shift))&mask
     else:
      for k in range(n):mem[(slot+k)&255]=read(255 if d==255 else d+k)
    elif d!=255:
     if size<4:
      bits=8 if size<2 else 16;x=(mem[slot]>>shift)&((1<<bits)-1)
      if size&1 and x&(1<<(bits-1)):x-=1<<bits
      reg[d]=x&mask
     else:
      for k in range(n):
       if d+k<255:reg[d+k]=mem[(slot+k)&255]
   elif w&0xfff0000000000000==0xe300000000000000:return reg
   elif w&0xfff0000000000020==0xe240000000000000:
    off=(w>>20)&0xffffff;off=off-(1<<24)if off&(1<<23)else off;pc=(pc*8+off)//8
   elif w&0xfff0000000000000==0x0100000000000000:reg[d]=(w>>20)&mask
   elif w&0xfe00000000000000==0x1c00000000000000:reg[d]=(read(base)+((w>>20)&mask))&mask
   elif w&0xfef0000000000000==0x3660000000000000:
    assert (w>>49)&7==5;ps[(w>>3)&7]=read(base)!=0
   else:raise AssertionError(hex(w))
  raise AssertionError('oracle loop did not terminate')
 rng=random.Random(0x4c4d454d);numeric=0;sanitized_vectors=0;vectors=open(out/'vectors.bin','wb')
 addresses=list(range(0,1024,4))+[1,2,3,1021,1022,1023,1024,1028,0xfffffffc,0xffffffff,0xfffffbfc]
 for n,(variant,mode,i,_,_)in enumerate(bodies):
  for addr in addresses:
   for pred in range(4):
    values=[rng.getrandbits(32)for _ in range(256)];values[255]=0;values[31]=addr;values[4]=addr;values[6]=addr
    want=reference(rows[i],values,pred);actual=(ctypes.c_uint32*256)(*values);lib.evaluate(n,actual,pred)
    assert list(actual)==want,(variant,mode,i,hex(addr),pred,next((k,hex(actual[k]),hex(want[k]))for k in range(256)if actual[k]!=want[k]))
    numeric+=1
    if addr%64==0 or addr>=1024 or addr%4:
     vectors.write(struct.pack('<514I',n,pred,*values,*want));sanitized_vectors+=1
 vectors.close()
 replay=cpp+r'''
#include <cstdio>
int main(int argc,char**argv){
 if(argc!=2)return 1;FILE*f=std::fopen(argv[1],"rb");if(!f)return 2;
 uint r[514];unsigned n=0;
 while(std::fread(r,sizeof(uint),514,f)==514){
  evaluate(r[0],r+2,r[1]);if(std::memcmp(r+2,r+258,256*sizeof(uint))){std::fprintf(stderr,"emitted replay mismatch case%u pred%u\n",r[0],r[1]);return 3;}n++;
 }
 std::fclose(f);std::printf("%u sanitized emitted-code vectors passed\n",n);return 0;
}
'''
 (out/'replay.cpp').write_text(replay)
 run(['clang++','-std=c++17','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',out/'replay.cpp','-o',out/'replay-sanitized'],'replay-build.log')
 run([out/'replay-sanitized',out/'vectors.bin'],'replay-run.log')
 report={'state':'passed','programs':len(rows),'modes':['nested','pc'],'actual_emitted_numeric_checks':numeric,'sanitized_emitted_vectors':sanitized_vectors,'spirv_shaders':validated,'unchanged_fallback_shaders':unchanged,
         'source_sha256':{str(p.relative_to(ROOT)):sha(p)for p in (SRC,SRC.with_name('shader.h'),TEST,Path(__file__))},'scope':'Actual emitted GLSL bodies executed as uint32 C++ with controlled input-register/predicate initialization; independent memory/address/control oracle. Synthetic instructions only, no GPU/device performance claim.'}
 (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
