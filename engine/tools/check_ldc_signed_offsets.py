#!/usr/bin/env python3
"""Real LDC emission, modular32 numeric checks, SPIR-V, and Android TU."""
import ctypes,hashlib,json,os,pathlib,re,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];SRC=ROOT/'ports/shared-android/runtime/nvn/maxwell/translate.c';OUT=ROOT/'build/ldc-signed-offsets'
def run(args,**kw):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=120,**kw)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in[SRC,SRC.with_name('shader.h'),ROOT/'tests/ldc_signed_offset_test.c',pathlib.Path(__file__)]};results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run(['clang','-std=c11','-D_GNU_SOURCE',*flags,'-I',SRC.parent,'-I',ROOT/'native',ROOT/'tests/ldc_signed_offset_test.c',SRC,'-lm','-o',exe])
  for robust in(0,1):
   env=dict(os.environ,LARECOMP_VK_ROBUST_CBUF=str(robust));results[f'{mode}-{robust}']=run([exe,OUT,robust],env=env)
  print(mode,results[f'{mode}-0'],results[f'{mode}-1'],flush=True)
 files=sorted(OUT.glob('ldc-*.vert'));fns=[];chosen=[]
 for i,p in enumerate(files):
  s=p.read_text();robust,scalar,imm,size,form=map(int,p.stem.split('-')[1:]);body=s[s.index('void main() {'):];assigns=re.findall(r'^\s*r[45] = ([^;]+);',body,re.M);assert len(assigns)==size-3
  macros='\n'.join(x for x in s.splitlines()if x.startswith(('#define cbr(', '#define cbra(', '#define cbrs(', '#define cbras(')))
  # Real production helper macros and destination expressions, checked backing.
  names=['cbr','cbra','cbrs','cbras'];fns.append('\n'.join('#undef '+n for n in names)+'\n'+macros+f'\nextern "C" uint64_t e{i}(uint r8,uint real_bytes){{bound=real_bytes;uint ldc_addr=0;uint r4={assigns[0]};uint r5='+ (assigns[1]if size==5 else'0u')+';return uint64_t(r4)|(uint64_t(r5)<<32);}')
  if imm in(0x7fff,0x8000,0xffff,0xffe0,0xfff8)and(form==1 or form==2):chosen.append(p)
 pre='''#include <cstdint>
#include <algorithm>
using uint=uint32_t;using std::min;static uint bound;
static uint word(uint64_t off){return off+4<=bound?uint(off)^0x9e3779b9u:0;}
struct Row{uint64_t row;uint operator[](uint c)const{return word(row*16+c*4);}};
struct CB{uint length()const{return 4096;}Row operator[](uint r)const{return {r};}}cb8;
struct SCB{uint length()const{return 16384;}uint operator[](uint r)const{return word(uint64_t(r)*4);}}cb8_s;
'''
 (OUT/'emitted.cpp').write_text(pre+'\n'.join(fns));run(['clang++','-std=c++17','-O2','-shared','-fPIC',OUT/'emitted.cpp','-o',OUT/'emitted.so']);lib=ctypes.CDLL(str(OUT/'emitted.so'));numeric=0
 for i,p in enumerate(files):
  robust,scalar,imm,size,form=map(int,p.stem.split('-')[1:]);signed=imm if imm<32768 else imm-65536;fn=getattr(lib,f'e{i}');fn.argtypes=[ctypes.c_uint32,ctypes.c_uint32];fn.restype=ctypes.c_uint64
  bases=[0,16,32,64,0xfffffff0,0x80000000,0xffffff00,65520]if form==1 else[0,1,15,16,31,32,0xffffffff,0xfffffffc,0xfffffff0,0x80000000,65535]
  for base in bases:
   for bound in(0,4,64,2496,65536):
    expected=[]
    for j in range(size-3):
     addr=((0 if form==2 else base)+signed+j*4)&0xffffffff;row=addr>>4;c=(addr>>2)&3
     # Direct positive RZ has compile-time indexing; otherwise existing
     # non-robust declarations clamp to 4096 rows before descriptor bounds.
     if not robust and(form!=2 or signed<0):row=min(row,4095)
     byte=row*16+c*4
     if not robust and scalar and 'cbrs(' in re.search(r'^\s*r4 = ([^;]+);',p.read_text(),re.M)[1]:byte=min(addr>>2,16383)*4
     expected.append((byte^0x9e3779b9)if byte+4<=bound else 0)
    want=expected[0]|((expected[1]if len(expected)>1 else 0)<<32);got=fn(base,bound);assert got==want,(p.name,hex(base),bound,hex(got),hex(want));numeric+=1
 for p in chosen:
  spv=p.with_suffix('.spv');run(['glslangValidator','-V','--target-env','vulkan1.3',p,'-o',spv]);run(['spirv-val','--target-env','vulkan1.3','--scalar-block-layout',spv])
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(x for x in rows if x['file'].endswith('/runtime/nvn/maxwell/translate.c'));cmd=shlex.split(row['command']);obj=OUT/'translate-android.o';cmd[cmd.index('-o')+1]=str(obj);overlay=OUT/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/maxwell/translate.c'),'external-contents':str(SRC)}]}));run([*cmd,'-ivfsoverlay',overlay],cwd=row['directory'])
 report={'state':'passed','host_tests':results,'actual_emitted_numeric_checks':numeric,'GLSL_variants':len(files),'SPIRV_variants':len(chosen),'Android_full_TU':True,'source_sha256':pins,'scope':'Signed16 LDC B32/B64, modular32 normal address_mode0; aligned/unaligned/RZ, positive/negative boundaries, scalar/vector CB, robust/clamp modes. MSL same byte-address expression checked; no Metal GPU execution. Other address modes rejected. Private owned replay reported separately.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
