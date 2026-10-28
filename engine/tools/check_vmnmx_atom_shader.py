#!/usr/bin/env python3
"""Verify actual VMNMX expressions and returning ATOM.ADD, optionally original shaders."""
import argparse,ctypes,hashlib,json,pathlib,random,re,shlex,shutil,struct,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');p.add_argument('--original-bfsha',type=pathlib.Path);a=p.parse_args();out=ROOT/'build/vmnmx-atom-shader';out.mkdir(parents=True,exist_ok=True);source=ROOT/'ports/shared-android/runtime/nvn/maxwell/translate.c';test=ROOT/'tests/vmnmx_atom_shader_test.c';paths=[source,source.with_name('shader.h'),test,pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x) for x in paths}
 def run(args,name):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=90);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 compilers={k:shutil.which(k)for k in ('clang','clang++','glslangValidator','spirv-val')};assert all(compilers.values())
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([compilers['clang'],'-std=c11','-D_GNU_SOURCE',*flags,'-I',ROOT/'native','-I',source.parent,test,source,'-lm','-o',exe],name+'-build.log');reports[name]=json.loads(run([exe,out],name+'-run.log'))
 actual=[]
 for stale in out.glob('original-*.comp'):stale.unlink()
 if a.original_bfsha:
  b=a.original_bfsha.read_bytes();needle=struct.pack('<Q',0x3b7707e061172a0f);at=0
  while True:
   at=b.find(needle,at)
   if at<0:break
   base=at-0x100-0xd28
   if base>=0 and b[base:base+4]==struct.pack('<I',0x12345679) and at+8<=len(b):
    data=b[base:base+0x4000];path=out/f'original-{base:x}.bin';path.write_bytes(data);name=f'original-{base:x}'
    for variant in reports:run([out/variant,out,path,name],variant+'-'+name+'.log')
    actual.append({'resource_sha256':sha(a.original_bfsha),'original_offset':base,'bounded_bytes':len(data),'shader_sha256':sha(path),'name':name,'address_int64_modes':[0,1],'zero_miss_modes':[0,1],'gmem_mirror':1,'gfind_cache':1,'chunk_shift':0})
   at+=8
  assert len(actual)==2,'Expected both independently matched original compute shader variants'
 words=[int(line.split()[1],16) for line in (out/'words.txt').read_text().splitlines()]+[0x3b7707e061172a0f];expressions=[]
 for i,w in enumerate(words):
  file=out/(f'case-{i:03d}.comp'if i<288 else'actual-vmnmx.comp');matches=re.findall(r'\br15 = ([^;\n]+);',file.read_text());assert matches;expressions.append(matches[-1])
 cpp='#include <cstdint>\n#include <algorithm>\nusing uint=uint32_t;using std::min;using std::max;\nextern "C" uint evaluate(unsigned i,uint a,uint b,uint c){uint r42=a,r17=b,r15=c;switch(i){\n'+''.join(f'case {i}:r15={expr};break;\n'for i,expr in enumerate(expressions))+'default:return 0;}return r15;}\n';(out/'expressions.cpp').write_text(cpp);run([compilers['clang++'],'-std=c++17','-O2','-shared','-fPIC',out/'expressions.cpp','-o',out/'expressions.so'],'expression-build.log');lib=ctypes.CDLL(str(out/'expressions.so'));lib.evaluate.argtypes=[ctypes.c_uint32]*4;lib.evaluate.restype=ctypes.c_uint32
 mask=0xffffffff
 def signed(v):return v if v<0x80000000 else v-0x100000000
 def operand(v,width,selector,sign):
  bits=32 if width==3 else 16 if width==2 else 8
  if bits==32:return v&mask
  v=(v>>(selector*bits))&((1<<bits)-1)
  return (v-(1<<bits)if sign and v&(1<<(bits-1))else v)&mask
 def reference(w,a,b,c):
  sb=bool((w>>49)&1);sa=bool((w>>48)&1);sd=bool((w>>54)&1);reg=bool((w>>50)&1);x=operand(a,(w>>37)&3,(w>>36)&3,sa);y=operand(b,(w>>29)&3,(w>>28)&3,sb)if reg else operand((w>>20)&0xffff,2,0,sb)
  first=(max if(w>>56)&1 else min)(signed(x)if sb else x,signed(y)if sb else y)&mask
  return (max if((w>>51)&7)==6 else min)(signed(first)if sd else first,signed(c)if sd else c)&mask
 rng=random.Random(852);edge=[0,1,0x7f,0x80,0x7fff,0x8000,0xffff,0x7fffffff,0x80000000,mask,0x123480ff];numeric=0
 for i,w in enumerate(words):
  vectors=[(x,edge[(j+3)%len(edge)],edge[(j+6)%len(edge)])for j,x in enumerate(edge)]+[tuple(rng.getrandbits(32)for _ in range(3))for _ in range(120)]
  for av,bv,cv in vectors:assert lib.evaluate(i,av,bv,cv)==reference(w,av,bv,cv),(i,hex(w),av,bv,cv);numeric+=1
 atom=(out/'actual-atom.comp').read_text();start=atom.index('if (fw != 0xFFFFFFFFu) { uint o_ = atomicCompSwap');start=atom.index('{',start);depth=1;end=start+1
 while depth:depth+=(atom[end]=='{')-(atom[end]=='}');end+=1
 body=atom[start+1:end-1].replace('gfast_.w[fw]','cell')
 cpu='''#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
typedef uint32_t uint;
static unsigned checks;
#define C(x) do{checks++;if(!(x))abort();}while(0)
static uint atomicCompSwap(uint*cell,uint compare,uint value){__atomic_compare_exchange_n(cell,&compare,value,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return compare;}
static uint emitted(uint*cell,uint r5){uint atom_value_=r5,atom_old_=0u;BODY r5=atom_old_;return r5;}
static uint cell,tickets[40000];
static void*worker(void*p){uintptr_t id=(uintptr_t)p;for(unsigned i=0;i<10000;i++)tickets[id*10000+i]=emitted(&cell,1);return NULL;}
static int compare(const void*a,const void*b){uint x=*(const uint*)a,y=*(const uint*)b;return(x>y)-(x<y);}
int main(void){cell=0xffffffffu;C(emitted(&cell,1)==0xffffffffu&&cell==0);cell=0x80000000u;C(emitted(&cell,0x80000000u)==0x80000000u&&cell==0);uint state=7;for(unsigned i=0;i<20000;i++){state=state*1664525u+1013904223u;cell=state;uint old=cell;state=state*1664525u+1013904223u;C(emitted(&cell,state)==old&&cell==old+state);}cell=0;pthread_t t[4];for(uintptr_t i=0;i<4;i++)C(!pthread_create(t+i,NULL,worker,(void*)i));for(unsigned i=0;i<4;i++)C(!pthread_join(t[i],NULL));C(cell==40000);qsort(tickets,40000,4,compare);for(unsigned i=0;i<40000;i++)C(tickets[i]==i);printf("{\\"checks\\":%u,\\"threads\\":4,\\"increments\\":40000,\\"unique_old_values\\":true}\\n",checks);return 0;}
'''.replace('BODY',body).replace('\\"','\"').replace('\\n','\n')
 # Keep C string literal escapes in the generated print statement.
 cpu=cpu.replace('printf("{"checks":%u,"threads":4,"increments":40000,"unique_old_values":true}\n",checks);','printf("{\\"checks\\":%u,\\"threads\\":4,\\"increments\\":40000,\\"unique_old_values\\":true}\\n",checks);')
 (out/'atomic-loop.c').write_text(cpu);atomic_reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined'])]:
  exe=out/('atomic-'+name);run([compilers['clang'],'-std=c11',*flags,out/'atomic-loop.c','-pthread','-o',exe],name+'-atomic-build.log');atomic_reports[name]=json.loads(run([exe],name+'-atomic-run.log'))
 selected=[out/'actual-vmnmx.comp',*[out/f'case-{i:03d}.comp'for i in (0,7,31,63,127,191,255,256,287)],*out.glob('atom-*.comp'),out/'actual-atom.comp',*out.glob('original-*.comp')]
 for file in selected:
  target=file.with_suffix('.spv');run([compilers['glslangValidator'],'-V','--target-env','vulkan1.3',file,'-o',target],file.stem+'-spirv-build.log');run([compilers['spirv-val'],'--target-env','vulkan1.3',target],file.stem+'-spirv-val.log')
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(x for x in rows if x['file'].endswith('/runtime/nvn/maxwell/translate.c'));cmd=shlex.split(row['command']);obj=out/'translate-android.o';cmd[cmd.index('-o')+1]=str(obj);overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/maxwell/translate.c'),'external-contents':str(source)}]}));r=subprocess.run([*cmd,'-ivfsoverlay',str(overlay)],cwd=row['directory'],capture_output=True,text=True,timeout=90);(out/'android-production-build.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr;android={'passed':True,'object_sha256':sha(obj)}
 assert pins=={k:sha(ROOT/k)for k in pins},'Source changed during verification'
 report={'state':'passed','host_tests':reports,'emitted_vmnmx_numeric_cases':numeric,'emitted_atom_contention':atomic_reports,'spirv_variants':len(selected),'actual_original_shaders':actual,'android_production_compile':android,'source_sha256':pins,'isa_reference':'Eden local immutable commit c6541721fa6307e99b3fdd9517dd4ed1b73927ed opcode and operand semantics; independent GLSL emission, no source incorporation','scope':'Actual emitted integer expressions/CAS loop on host and original resource translation+SPIR-V validation; no GPU execution or visual correctness claim.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
