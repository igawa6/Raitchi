#!/usr/bin/env python3
"""Check four narrow Echoes shader deltas using actual translation and emitted-expression execution."""
import ctypes
import hashlib
import json
from pathlib import Path
import random
import re
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/"ports/shared-android/runtime/nvn/maxwell/translate.c"
OUT=ROOT/"build/echoes-shaders"

def run(args):
    p=subprocess.run([str(x) for x in args],capture_output=True,text=True,timeout=45)
    if p.returncode:raise RuntimeError(f"{args[0]} failed:\n{p.stdout}{p.stderr}")
    return p.stdout.strip()

def main():
    OUT.mkdir(parents=True,exist_ok=True);cc=shutil.which('clang');cxx=shutil.which('clang++')
    glslang=shutil.which('glslangValidator');validator=shutil.which('spirv-val')
    if not all((cc,cxx,glslang,validator)):raise RuntimeError('Installed clang/clang++/glslangValidator/spirv-val required; no downloads')
    tests=ROOT/'tests/echoes_shader_translate_test.c';results={}
    for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        runner=OUT/name
        run([cc,'-std=c11','-D_GNU_SOURCE',*flags,'-I',SOURCE.parent,'-I',ROOT/'native',tests,SOURCE,'-lm','-o',runner])
        results[name]=run([runner,OUT]);print(name+': '+results[name],flush=True)
    files=sorted(OUT.glob('*.glsl'));expressions=[]
    for index,path in enumerate(files):
        src=path.read_text();match=re.search(r'^\s*r0 = ([^;]+);',src,re.M)
        if not match:raise RuntimeError('Missing actual emitted destination expression '+path.name)
        expressions.append('extern "C" uint e'+str(index)+'(uint r8,uint r9,uint r10){return '+match[1]+';}')
        spv=path.with_suffix('.spv');run([glslang,'-V','--target-env','vulkan1.3','-S','comp',path,'-o',spv]);run([validator,'--target-env','vulkan1.3',spv])
    example=files[0].read_text();helpers=[]
    for name in ['clz','brev32','flo_sh','bfe_u','bfe_s']:
        match=re.search(r'uint '+name+r'\([^\n]+',example)
        if not match:raise RuntimeError('Missing emitted helper '+name)
        helpers.append(match[0])
    builtins='''#include <cstdint>
#include <algorithm>
using uint=uint32_t;using std::min;
static int findMSB(uint v){if(!v)return -1;int n=0;while(v>>=1)n++;return n;}
static uint bitfieldReverse(uint v){uint r=0;for(int i=0;i<32;i++){r=(r<<1)|(v&1);v>>=1;}return r;}
static uint bitfieldExtract(uint v,int p,int n){if(n==32)return v;return (v>>p)&((uint(1)<<n)-1);}
static int bitfieldExtract(int v,int p,int n){uint r=bitfieldExtract(uint(v),p,n);if(n<32&&(r&(uint(1)<<(n-1))))r|=~((uint(1)<<n)-1);return int(r);}
'''
    source=OUT/'emitted.cpp';source.write_text(builtins+'\n'.join(helpers+expressions));library=OUT/'emitted.so';run([cxx,'-std=c++17','-shared','-fPIC','-O2',source,'-o',library]);lib=ctypes.CDLL(str(library))
    rng=random.Random(9431);values=[0,1,2,0x80000000,0xffffffff,0x7fffffff,0x40000000]+[rng.getrandbits(32) for _ in range(100)];checks=0
    for index,path in enumerate(files):
        fn=getattr(lib,'e'+str(index));fn.argtypes=[ctypes.c_uint32]*3;fn.restype=ctypes.c_uint32
        fields=path.stem.split('-')
        for a in values:
            for b,c in [(rng.getrandbits(32),rng.getrandbits(32)),(0,0),(0xffffffff,0xffffffff),(0x2000,0x80000000)]:
                if fields[0]=='bfe':
                    sign,brev=map(int,fields[1:]);v=int(f'{a:032b}'[::-1],2) if brev else a;p=b&255;n=(b>>8)&255
                    if not n:expected=0
                    elif p>=32:expected=0xffffffff if sign and v>>31 else 0
                    else:
                        n=min(n,32-p);expected=(v>>p)&((1<<n)-1)
                        if sign and expected>>(n-1):expected|=~((1<<n)-1)
                elif fields[0]=='flo':
                    sign,inv,shift=map(int,fields[1:]);v=(~b)&0xffffffff if inv else b
                    if sign and v>>31:v=(~v)&0xffffffff
                    expected=v.bit_length()-1 if v else 0xffffffff
                    if shift and expected!=0xffffffff:expected=31-expected
                else:
                    shift=31 if fields[1]=='zero' else int(fields[1]);high=0 if fields[1]=='zero' else c
                    expected=((((high<<32)|a)<<shift)>>32)+b
                got=fn(a,b,c);expected&=0xffffffff
                if got!=expected:raise RuntimeError(f'{path.stem}: {a:x},{b:x},{c:x}: {got:x}!={expected:x}')
                checks+=1
    print(f'{len(files)} actual emitted GLSL programs compiled/validated; {checks} emitted-expression comparisons passed',flush=True)
    tracked=[SOURCE,tests,Path(__file__),SOURCE.with_name('shader.h')]
    report={'schema':1,'scope':'Actual translator/header tests optimized+ASan/UBSan; 45 synthetic Maxwell programs, emitted GLSL/SPIR-V compilation/validation and execution of actual emitted scalar expressions with controlled shader built-ins. Not GPU or game execution.','source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked},'results':results,'glsl_spirv_programs':len(files),'emitted_expression_checks':checks,'device_actions':False}
    (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
