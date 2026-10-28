#!/usr/bin/env python3
"""Check mixed F32/F16 arithmetic through the actual shader emitter; no game assets required."""
import argparse,ctypes,hashlib,json,math,os,pathlib,random,re,shlex,shutil,struct,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--android',action='store_true');ap.add_argument('--roof-bfsha',type=pathlib.Path);args=ap.parse_args()
 out=ROOT/'build/mixed-half-shader';out.mkdir(parents=True,exist_ok=True);source=ROOT/'ports/shared-android/runtime/nvn/maxwell/translate.c';test=ROOT/'tests/mixed_half_shader_test.c';pins={str(p.relative_to(ROOT)):sha(p)for p in [source,source.with_name('shader.h'),test,pathlib.Path(__file__).resolve()]}
 env=dict(os.environ,LARECOMP_VK_HALF_SHADOW='0');reports={}
 def run(command,name,**kw):
  r=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=120,env=kw.pop("env",env),**kw);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 for tool in ['clang','clang++','glslangValidator','spirv-val','spirv-dis']:assert shutil.which(tool),tool
 common=['clang','-std=c11','-D_GNU_SOURCE','-I',ROOT/'native','-I',source.parent]
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'])]:
  exe=out/mode;run(common+flags+[test,source,'-lm','-o',exe],mode+'-build.log');reports[mode]={}
  for native in [0,1]:
   d=out/('native'if native else'software');d.mkdir(exist_ok=True);reports[mode][str(native)]=json.loads(run([exe,d,native],mode+f'-{native}-run.log'))
 rows=[list(map(lambda x:int(x,16)if len(x)==16 else int(x),s.split()))for s in (out/'native/index.txt').read_text().splitlines()]
 # Evaluate expressions copied from the ACTUAL emission, not a separately reimplemented emitter.
 exprs=[]
 for native in [0,1]:
  d=out/('native'if native else'software')
  for row in rows:
   if row[-1]:
    matches=re.findall(r'^\s*r0 = (.*);$',(d/f'{row[0]:03d}.comp').read_text(),re.M);assert len(matches)==1
    exprs.append((native,row,matches[0]))
 preamble=r'''
#include <cstdint>
#include <cmath>
#include <cstring>
using uint=uint32_t;
struct V{float x,y;}; static bool native;
static float asf(uint x){float r;memcpy(&r,&x,4);return r;}
static uint asu(float x){uint r;memcpy(&r,&x,4);return r;}
static float half(float x){return float((_Float16)x);}
static V vec2(float x){return{x,x};} static V vec2(V x){return x;}
static V f16vec2(V v){return{half(v.x),half(v.y)};}
static uint packHalf2x16(V x){_Float16 a=(_Float16)x.x,b=(_Float16)x.y;uint16_t lo,hi;memcpy(&lo,&a,2);memcpy(&hi,&b,2);return lo|(uint(hi)<<16);}
static V unpackHalf2x16(uint v){uint16_t lo=v,hi=v>>16;_Float16 a,b;memcpy(&a,&lo,2);memcpy(&b,&hi,2);return{float(a),float(b)};}
static V hx(uint v,uint swz){if(swz==1)return vec2(native?half(asf(v)):asf(v));V h=unpackHalf2x16(v);return swz==2?vec2(h.x):swz==3?vec2(h.y):h;}
static V operator+(V a,V b){return{a.x+b.x,a.y+b.y};} static V operator*(V a,V b){return{a.x*b.x,a.y*b.y};} static V operator-(V a){return{-a.x,-a.y};}
static V fma(V a,V b,V c){return{std::fma(a.x,b.x,c.x),std::fma(a.y,b.y,c.y)};}
static V saturate(V a){return{std::isnan(a.x)?0.f:std::fmin(1.f,std::fmax(0.f,a.x)),std::isnan(a.y)?0.f:std::fmin(1.f,std::fmax(0.f,a.y))};}
static uint hpack(V v,uint merge,uint old){if(merge==1)return asu(v.x);uint p=packHalf2x16(v);return merge==2?(old&0xffff0000)|(p&0xffff):merge==3?(old&0xffff)|(p&0xffff0000):p;}
static float uintBitsToFloat(uint x){return asf(x);}
static uint floatBitsToUint(float x){return asu(x);}
extern "C" uint evaluate(unsigned i,uint r0,uint r1,uint r2,uint r3){switch(i){
'''
 cpp=preamble+''.join(f'case {i}:native={n};return {expr};\n'for i,(n,row,expr)in enumerate(exprs))+'default:return0;} }'.replace('return0','return 0')
 (out/'emitted.cpp').write_text(cpp);run(['clang++','-std=c++17','-O2','-ffp-contract=off','-shared','-fPIC',out/'emitted.cpp','-o',out/'emitted.so'],'emitted-build.log')
 lib=ctypes.CDLL(str(out/'emitted.so'));lib.evaluate.argtypes=[ctypes.c_uint32]*5;lib.evaluate.restype=ctypes.c_uint32
 libc=ctypes.CDLL(None);libc.fmaf.argtypes=[ctypes.c_float]*3;libc.fmaf.restype=ctypes.c_float
 f32=lambda x:ctypes.c_float(x).value
 def h(x):
  try:return struct.unpack('<e',struct.pack('<e',x))[0]
  except OverflowError:return math.copysign(math.inf,x)
 u=lambda x:struct.unpack('<I',struct.pack('<f',x))[0]
 hf=lambda lo,hi:struct.unpack('<I',struct.pack('<ee',lo,hi))[0]
 def operand(v,sw):
  if sw==1:return[f32(struct.unpack('<f',struct.pack('<I',v))[0])]*2
  lo,hi=struct.unpack('<ee',struct.pack('<I',v));return[lo,hi]if sw==0 else[lo if sw==2 else hi]*2
 def reference(row,old,av,bv,cv):
  _,kind,a,b,c,merge,sat,neg,word,mixed=row;x=operand(av,a);y=operand(bv,b);z=operand(cv,c);v=[]
  for i in range(2):
   yy=-y[i]if neg else y[i]
   r=f32(x[i]+yy)if kind==0 else f32(x[i]*yy)if kind==1 else libc.fmaf(x[i],yy,z[i])
   if sat:r=0.0 if math.isnan(r)else min(1.0,max(0.0,r))
   all_f32=a==1 and b==1 and(kind!=2 or c==1)
   v.append(r if all_f32 and merge==1 else h(r))
  packed=hf(*v)
  return u(v[0])if merge==1 else(old&0xffff0000)|(packed&0xffff)if merge==2 else(old&0xffff)|(packed&0xffff0000)if merge==3 else packed
 rng=random.Random(0x4d49584544);numeric=0;by_mode={};counterexample={}
 for i,(native,row,expr)in enumerate(exprs):
  key=row[0];vectors=[]
  for j in range(96):
   vv=[]
   for sw in row[2:5]:
    if sw==1:vv.append(u(rng.uniform(-8,8)))
    else:vv.append(hf(h(rng.uniform(-4,4)),h(rng.uniform(-4,4))))
   vectors.append((0x3555bc00,*vv))
  # Exact cancellation and half-way boundaries, with subnormal half results retained.
  for e in [-20,-15,-11,-10,-1,0,2]:
   vv=[]
   for pos,sw in enumerate(row[2:5]):
    value=(1+2.0**e)if pos==0 else(-1.0 if row[1]==0 or pos==2 else 1.0)
    vv.append(u(value)if sw==1 else hf(h(value),h(value)))
   vectors.append((0x3555bc00,*vv))
  results=[]
  for old,av,bv,cv in vectors:
   got=lib.evaluate(i,old,av,bv,cv);want=reference(row,old,av,bv,cv);assert got==want,(native,row,hex(got),hex(want),av,bv,cv);numeric+=1;results.append(got)
  # Identical deterministic boundary vectors must agree in both capability modes.
  by_mode[(native,key)]=results[-7:]
 for native,row,expr in exprs:
  if native:assert by_mode[(native,row[0])]==by_mode[(0,row[0])]
 # Execute actual emitted cbuf comparison blocks against an independent oracle.
 compare_rows=[list(map(int,line.split()[:-1]))+[int(line.split()[-1],16)]
               for line in (out/'native/compare-index.txt').read_text().splitlines()]
 compare_blocks=[]
 for native in [0,1]:
  d=out/('native'if native else'software')
  for row in compare_rows:
   text=(d/f'cmp{row[0]:04d}.comp').read_text()
   block=re.search(r'\{ vec2 hx_ = .*?\n    \}',text,re.S);assert block,row
   body=re.sub(r'cb\d+\[\d+\]\[\d+\]','cv',block[0]).replace('{ vec2 hx_','{ V hx_')
   compare_blocks.append((native,row,body))
 compare_cpp=preamble.split('extern "C" uint evaluate')[0]+r'''
using std::isnan; using std::abs;
static V abs(V x){return{std::fabs(x.x),std::fabs(x.y)};}
extern "C" uint64_t compare_emitted(unsigned i,uint r0,uint r1,uint cv,uint predicates){
bool p0=predicates&1u,p1=predicates&2u,p2=predicates&4u;
switch(i){
'''+''.join(f'case {i}:{{native={str(bool(n)).lower()};{block}\nreturn uint64_t(r0)|(uint64_t(p0)<<32)|(uint64_t(p1)<<33);}}\n'
            for i,(n,row,block)in enumerate(compare_blocks))+'default:return 0;} }'
 (out/'compare-emitted.cpp').write_text(compare_cpp)
 run(['clang++','-std=c++17','-O2','-ffp-contract=off','-shared','-fPIC',out/'compare-emitted.cpp','-o',out/'compare-emitted.so'],'compare-emitted-build.log')
 compare_lib=ctypes.CDLL(str(out/'compare-emitted.so'));compare_lib.compare_emitted.argtypes=[ctypes.c_uint32]*5;compare_lib.compare_emitted.restype=ctypes.c_uint64
 def fp_compare(op,a,b):
  unordered=math.isnan(a)or math.isnan(b)
  if op==0:return False
  if op==15:return True
  if op==7:return not unordered
  if op==8:return unordered
  if unordered:return op>=9
  operation=op-8 if op>=9 else op
  return (a<b,a==b,a<=b,a>b,a!=b,a>=b)[operation-1]
 def compare_reference(row,old,av,bv,pbits):
  _,setp,cop,bop,sw,flag,na,aa,nb,ab,ftz,np,pred,pa,pb,word=row
  a=operand(av,sw);b=operand(bv,1)
  a=[(-1 if na else 1)*(abs(x)if aa else x)for x in a]
  b=[(-1 if nb else 1)*(abs(x)if ab else x)for x in b]
  if ftz:
   a=[0.0 if abs(x)<2**-126 else x for x in a];b=[0.0 if abs(x)<2**-126 else x for x in b]
  pp=True if pred==7 else bool(pbits&(1<<pred))
  if np:pp=not pp
  result=[]
  for x,y in zip(a,b):
   value=fp_compare(cop,x,y);result.append(value and pp if bop==0 else value or pp if bop==1 else value!=pp)
  predicates=[bool(pbits&(1<<i))for i in range(7)]
  if setp:
   if flag:result=[all(result),not all(result)]
   if pa!=7:predicates[pa]=result[0]
   if pb!=7:predicates[pb]=result[1]
  elif pa!=255:
   mask=0x3c00 if flag else 0xffff;old=(mask if result[0]else 0)|((mask if result[1]else 0)<<16)
  return old|(int(predicates[0])<<32)|(int(predicates[1])<<33)
 compare_numeric=0;compare_boundaries={}
 for i,(native,row,block)in enumerate(compare_blocks):
  sw=row[4]
  pack_source=lambda lo,hi:u(lo)if sw==1 else hf(lo,hi)
  avs=[pack_source(0.0,-0.0),pack_source(1.0,2.0),pack_source(-1.0,-2.0),
       pack_source(float('nan'),1.0),pack_source(1.0,float('nan'))]
  bvs=[u(x)for x in [0.0,1.0,-1.0,float('nan'),math.inf,-math.inf,1+2**-11]]
  vectors=[(a,b)for a in avs for b in bvs]
  vectors += [(pack_source(1+2**-11 if sw==1 else 1.0,1.0),u(1.0)),
              (pack_source(1.0,1.0),u(1+2**-11)),(pack_source(2**-24,0.0),1),
              (pack_source(1.0,1.0),0x007fffff),(pack_source(1.0,1.0),0x00800000),
              (1 if sw==1 else pack_source(2**-24,0.0),0),
              (pack_source(65520 if sw==1 else 65504,-65504),u(65520))]
  results=[]
  for av,bv in vectors:
   for pbits in [0,1,4,7]:
    old=0x3555bc00;got=compare_lib.compare_emitted(i,old,av,bv,pbits);want=compare_reference(row,old,av,bv,pbits)
    assert got==want,(native,row,hex(av),hex(bv),pbits,hex(got),hex(want));compare_numeric+=1;results.append(got)
  compare_boundaries[(native,row[0])]=results
 for row in compare_rows:assert compare_boundaries[(0,row[0])]==compare_boundaries[(1,row[0])]
 counterexample={'operation':'mixed F32(1+2^-11) + F16(-1), precision=None, no saturation','correct_result':2**-11,'premature_F32_to_F16_result':h(h(1+2**-11)-1),'expected_half_bits':'1000'}
 assert counterexample['premature_F32_to_F16_result']==0
 # All-half and ambiguous packed all-F32 paths remain byte-for-byte unchanged.
 baseline=source.read_text().replace('bool mixed = c->glsl &&','bool mixed = false &&');assert baseline!=source.read_text();(out/'baseline.c').write_text(baseline);(out/'baseline_test.c').write_text(test.read_text().replace('if(mixed) {','if(false) {').replace('CHECK(strstr(s->msl,"r0 = floatBitsToUint((fma(vec2(uintBitsToFloat(r1))")!=NULL);',''))
 run(common+['-O2',out/'baseline_test.c',out/'baseline.c','-lm','-o',out/'baseline'],'baseline-build.log');unchanged=0;negative=0
 for native in [0,1]:
  d=out/f'baseline-{native}';d.mkdir(exist_ok=True);run([out/'baseline',d,native],f'baseline-{native}-run.log');current=out/('native'if native else'software')
  for row in rows:
   same=(d/f'{row[0]:03d}.comp').read_bytes()==(current/f'{row[0]:03d}.comp').read_bytes()
   if not row[-1]:assert same;unchanged+=1
   elif not same:negative+=1
 assert negative>0
 old_cases=[];old_rows=[]
 for kind,swb,swc in [(0,2,0),(2,1,1)]:
  row=next(r for r in rows if r[1]==kind and r[2:4]==[1,swb] and(kind!=2 or r[4]==swc) and r[5:8]==[1,0,0])
  text=(out/'baseline-1'/f'{row[0]:03d}.comp').read_text();expr=re.findall(r'^\s*r0 = (.*);$',text,re.M)[0];old_cases.append(expr);old_rows.append(row)
 oldcpp=preamble+''.join(f'case {i}:native=true;return {expr};\n'for i,expr in enumerate(old_cases))+'default:return 0;} }'
 (out/'old-emitted.cpp').write_text(oldcpp);run(['clang++','-std=c++17','-O2','-ffp-contract=off','-shared','-fPIC',out/'old-emitted.cpp','-o',out/'old-emitted.so'],'old-emitted-build.log');oldlib=ctypes.CDLL(str(out/'old-emitted.so'));oldlib.evaluate.argtypes=[ctypes.c_uint32]*5;oldlib.evaluate.restype=ctypes.c_uint32
 actual_counterexamples=[]
 for i,row in enumerate(old_rows):
  av=u(1+2**-11);bv=hf(-1,-1)if row[1]==0 else u(1);cv=hf(0,0)if row[1]==0 else u(-1);old=oldlib.evaluate(i,0,av,bv,cv);newi=next(j for j,(n,r,e)in enumerate(exprs)if n==1 and r[0]==row[0]);new=lib.evaluate(newi,0,av,bv,cv);assert old==u(0)and new==u(2**-11);actual_counterexamples.append({'kind':'mixed-add'if row[1]==0 else'all-F32-FMA','old_result_bits':f'{old:08x}','fixed_result_bits':f'{new:08x}'})
 # Validate representative actual shaders and assert the F32 operation precedes narrowing.
 checked=[];fma_proofs=[]
 for native in [0,1]:
  d=out/('native'if native else'software')
  for idx in [0,4,16,20,22,64,80,84,86,128,144,148,150,160,'guarded']:
   path=d/(f'{idx:03d}.comp' if isinstance(idx,int) else idx+'.comp');spv=path.with_suffix('.spv');run(['glslangValidator','-V','--target-env','vulkan1.3',path,'-o',spv],f'{native}-{idx}-spirv-build.log');run(['spirv-val','--target-env','vulkan1.3',spv],f'{native}-{idx}-spirv-val.log');asm=run(['spirv-dis',spv],f'{native}-{idx}.spvasm');checked.append(str(path.relative_to(out)))
   if idx==128 and native:
    m=re.search(r'(%\w+) = OpExtInst %v2float %\w+ Fma ',asm);assert m and re.search(r'OpFConvert %v2half '+re.escape(m[1])+r'\b',asm);fma_proofs.append({'native_half':native,'operation':'FP32 vector Fma','result':'FP16 vector FConvert after Fma'})
 compare_spirv=[]
 for native in [0,1]:
  d=out/('native'if native else'software')
  selected=[kind*384+cop*24+(cop%3)*8+((cop+1)%4)*2+cop%2 for kind in [0,1]for cop in range(16)]
  selected += [768,769,770,771,832,833,834,835,896,897,904,905]
  for idx in selected:
   path=d/f'cmp{idx:04d}.comp';spv=path.with_suffix('.spv')
   run(['glslangValidator','-V','--target-env','vulkan1.3',path,'-o',spv],f'compare-{native}-{idx}-build.log')
   run(['spirv-val','--target-env','vulkan1.3',spv],f'compare-{native}-{idx}-val.log');compare_spirv.append(str(path.relative_to(out)))
 # The shipped default keeps native-half register shadows; validate the same
 # new forms under that path as well as the numeric no-shadow capability pair.
 shadow_dir=out/'native-shadow';shadow_dir.mkdir(exist_ok=True)
 reports['optimized']['native-shadow']=json.loads(run([out/'optimized',shadow_dir,1,'compare-only'],'native-shadow-run.log',
                                                    env=dict(env,LARECOMP_VK_HALF_SHADOW='1')))
 for idx in [0,2,384,386,768,832]:
  path=shadow_dir/f'cmp{idx:04d}.comp';spv=path.with_suffix('.spv')
  run(['glslangValidator','-V','--target-env','vulkan1.3',path,'-o',spv],f'compare-shadow-{idx}-build.log')
  run(['spirv-val','--target-env','vulkan1.3',spv],f'compare-shadow-{idx}-val.log');compare_spirv.append(str(path.relative_to(out)))
 android={}
 if args.android:
  rowsc=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(x for x in rowsc if x['file'].endswith('/runtime/nvn/maxwell/translate.c'));cmd=shlex.split(row['command']);obj=out/'translate-android.o';cmd[cmd.index('-o')+1]=str(obj);overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/maxwell/translate.c'),'external-contents':str(source)},{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/maxwell/shader.h'),'external-contents':str(source.with_name('shader.h'))}]}));run([*cmd,'-ivfsoverlay',overlay],'android-production-build.log',cwd=row['directory']);android={'passed':True,'object_sha256':sha(obj)}
 original={}
 if args.roof_bfsha:
  b=args.roof_bfsha.read_bytes();base=0xa8e00;span=b[base:base+8764];assert len(span)==8764
  # This optional identity is independently matched to the actual material's full-span FNV.
  fnv=0xcbf29ce484222325
  for x in span:fnv=((fnv^x)*0x100000001b3)&0xffffffffffffffff
  assert fnv==0xcb65de2581d3ae79
  helper=out/'roof-translate.c';helper.write_text(r'''#include "shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
bool shared_title_las(void){return false;}
int main(int argc,char**argv){if(argc!=3)return 1;FILE*f=fopen(argv[1],"rb");if(!f)return 2;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<0xab03c)return 3;uint8_t*b=calloc(1,(size_t)n+4096);if(!b||fread(b,1,n,f)!=(size_t)n)return 4;fclose(f);maxwell_gmem_base=0x1240000000ull;maxwell_gmem_size=0xd0000000;maxwell_gmem_mirror=1;maxwell_shader_int64=0;maxwell_h2f_bits=0;maxwell_half_native=1;maxwell_scalar_cbuf=1;maxwell_gfind_cache=1;maxwell_gmem_zero=1;maxwell_clip_distance=1;maxwell_brx_table=false;maxwell_vote_stages=~0u;ShaderKey v={.glsl=1,.flip_y=1},k={.glsl=1,.flip_y=1};ShaderInfo*vi=maxwell_translate(b+0xa4100,10752,STAGE_VERTEX,&v);if(!vi||!vi->msl)return 5;k.vs_out_mask=vi->out_mask;ShaderInfo*fi=maxwell_translate(b+0xa8e00,8704,STAGE_FRAGMENT,&k);if(!fi||!fi->msl){fprintf(stderr,"%s",fi?fi->error:"allocation");return 6;}char path[1024];snprintf(path,sizeof path,"%s/roof.frag",argv[2]);f=fopen(path,"w");if(!f||fputs(fi->msl,f)<0||fclose(f))return 7;maxwell_free(fi);maxwell_free(vi);free(b);return 0;}
''')
  run(common+['-O2',helper,source,'-lm','-o',out/'roof-translator'],'roof-build.log');run([out/'roof-translator',args.roof_bfsha,out],'roof-translation.log');path=out/'roof.frag';run(['glslangValidator','-V','--target-env','vulkan1.3',path,'-o',out/'roof.spv'],'roof-spirv-build.log');run(['spirv-val','--target-env','vulkan1.3','--scalar-block-layout',out/'roof.spv'],'roof-spirv-val.log');shadow=out/'roof-shadow-default';shadow.mkdir(exist_ok=True);run([out/'roof-translator',args.roof_bfsha,shadow],'roof-shadow-translation.log',env=dict(env,LARECOMP_VK_HALF_SHADOW='1'));run(['glslangValidator','-V','--target-env','vulkan1.3',shadow/'roof.frag','-o',shadow/'roof.spv'],'roof-shadow-spirv-build.log');run(['spirv-val','--target-env','vulkan1.3','--scalar-block-layout',shadow/'roof.spv'],'roof-shadow-spirv-val.log');original={'resource_sha256':sha(args.roof_bfsha),'full_span_fnv':f'{fnv:016x}','shader_sha256':hashlib.sha256(span).hexdigest(),'shader_bytes':len(span),'translation_and_spirv_valid':True,'half_shadow_modes':[0,1]}
 assert pins=={k:sha(ROOT/k)for k in pins},'Source changed during verification'
 report={'state':'passed','host':reports,'half_cbuf':{'actual_emitted_numeric_checks':compare_numeric,'fixtures_per_capability':len(compare_rows),'spirv_shaders':compare_spirv,'native_software_results_agree':True,'reference':'yuzu Emulator Project (2021), GPL-2.0-or-later: Maxwell HSET2_cbuf/HSETP2_cbuf, Extract, FloatingPointCompare and PredicateCombine. Independent emission and fixtures; no reference implementation copied.','scope':'F32 cbuf splat, all compare/Boolean/A-swizzle/output flags, abs/neg, ordered and unordered NaNs, FTZ, predicate aliases, PT/RZ, slot/offset metadata, both half capability modes and default native-half shadows. No GPU/pixel correctness claim.'},'actual_emitted_numeric_checks':numeric,'all_half_and_packed_f32_unchanged_shaders':unchanged,'mixed_shader_counterfactuals_changed':negative,'cancellation_counterexample':counterexample,'actual_old_emitter_counterexamples':actual_counterexamples,'native_and_software_boundary_agreement':True,'spirv_shaders':checked,'spirv_dataflow':fma_proofs,'android':android,'optional_original_roof':original,'source_sha256':pins,'reference':'Eden mirror305f40100d76ec57b13c2f03eafd490db83691cb half_floating_point_{add,multiply,fused_multiply_add,helper}.cpp: mixed sources widen to F32; mixed results narrow to F16 before merge. All-F32 arithmetic stays F32 and mergeF32 preserves the result; half-lane merges narrow. Independent emission only; no GPL implementation copied.','limits':'No GPU execution or pixel causation claim. Numerical host oracle uses RTE; SPIR-V has no new explicit rounding-mode decoration, matching the inherited native-half execution controls. All-half and packed all-F32 paths are unchanged, not newly certified. All-F32 F32-result merge preserves FP32 output; half-lane merges narrow the result. Existing FTZ/FMZ handling is outside this mixed precision=None proof.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
