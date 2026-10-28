#!/usr/bin/env python3
"""Actual typed stencil translation/key regressions and whole Android compile."""
import argparse,hashlib,json,pathlib,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');p.add_argument('--original',nargs=5,action='append',default=[],metavar=('FILE','OFFSET','BYTES','MASK','NAME'));a=p.parse_args()
 out=ROOT/'build/stencil-sampler';out.mkdir(parents=True,exist_ok=True)
 runtime=ROOT/'ports/shared-android/runtime';source=runtime/'nvn/maxwell/translate.c';exec_=runtime/'nvn/vulkan/exec.c';header=source.with_name('shader.h');tests=[ROOT/'tests/stencil_sampler_test.c',ROOT/'tests/stencil_key_test.c',ROOT/'tests/stencil_binding_test.c'];paths=[source,exec_,header,*tests,pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x)for x in paths}
 s=exec_.read_text();start=s.index('static uint32_t sampled_uint_mask(');end=s.index('// Render-thread diagnostic only.',start);fragment=s[start:end]
 # Match the function definition (not an earlier declaration). Diagnostic
 # helpers may be inserted between the stencil helpers and this memo wrapper.
 start=s.index('static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {');end=s.index('static Prog *get_program_(NvnProgram *np,',start);fragment+=s[start:end]
 start=s.index('    uint64_t h = 0xcbf29ce484222325ull;',end);end=s.index('    uint32_t slot =',start)
 fragment+='static uint64_t production_program_hash(NvnProgram *np,ShaderKey*vkey,ShaderKey*fkey){\n'+s[start:end]+'return h;}\n'
 (out/'stencil_keys.production.h').write_text(fragment)
 start=s.index('static bool sampled_texture_handle(');end=s.index('static NvnTexture *shader_texture_object(',start);handle=s[start:end]
 start=s.index('static bool legacy_stencil_depth_view(');end=s.index('// Render-thread diagnostic only.',start);binding=s[start:end]
 texture_start=s.index('static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2], float fr[2], uint64_t shader_hash) {',end)
 texture_end=s.index('// Storage-image writes use real descriptor-pool textures,',texture_start)
 texture=s[texture_start:texture_end]
 trace_call='    sampling_trace_binding(stage, shader_hash, h, st_, ent, trace_sampler, sampler_default, null_view_fallback);'
 assert trace_call in texture
 # The binding regression exercises the real descriptor selection body. Keep
 # the diagnostic call's source assertion, while leaving its separately
 # tested logging helper out of this harness's dependency graph.
 texture=texture.replace(trace_call,'    (void)shader_hash; (void)trace_sampler; (void)sampler_default; (void)null_view_fallback;')
 (out/'stencil_binding.production.h').write_text(handle+binding+texture)
 assert 'vkey.tex_uint = sampled_uint_mask(0);'in s and'fkey.tex_uint = sampled_uint_mask(1);'in s
 assert 'st_->is_uint != vk_sampled_stencil(e->tex, &e->view, e->has_view)'in s
 assert 'feedback_sample_view(k, st_->type, st_->is_uint)'in s
 assert 'legacy stencil/comparison compatibility, accuracy unqualified'in s
 assert 'legacy stencil/comparison real depth view unavailable; refusing dummy'in s
 assert 'st_->is_uint ? vk_sampler_uint(d) : vk_sampler(d)'in s
 def run(args,log,cwd=None):
  r=subprocess.run(list(map(str,args)),cwd=cwd,capture_output=True,text=True,timeout=90);(out/log).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(log+': '+r.stdout+r.stderr)
  return r.stdout
 reports={};cc=shutil.which('clang')
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  run([cc,'-std=c11','-D_GNU_SOURCE',*flags,'-I',ROOT/'native','-I',source.parent,tests[0],source,'-lm','-o',out/name],name+'-build.log')
  original_args=[part for spec in a.original for part in spec]
  reports[name]={'translator':json.loads(run([out/name,out,*original_args],name+'-run.log'))}
  exe=out/(name+'-keys');run([cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,'-I',source.parent,tests[1],'-o',exe],name+'-keys-build.log');reports[name]['keys']=json.loads(run([exe],name+'-keys-run.log'))
  exe=out/(name+'-binding');run([cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,'-I',source.parent,tests[2],'-o',exe],name+'-binding-build.log');reports[name]['binding']=json.loads(run([exe],name+'-binding-run.log'))
 shader_files=list(out.glob('*.frag'))
 for file in shader_files:
  target=file.with_suffix('.spv');run([shutil.which('glslangValidator'),'-V','--target-env','vulkan1.3',file,'-o',target],file.stem+'-spirv.log');run([shutil.which('spirv-val'),'--target-env','vulkan1.3',target],file.stem+'-val.log')
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text())
  overlay=out/'overlay.json';owned=[source,exec_,header,runtime/'nvn/vulkan/vk.h'];overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/f.relative_to(runtime)),'external-contents':str(f)}for f in owned]}))
  for f in (source,exec_):
   row=next(r for r in rows if r['file'].endswith('/runtime/'+str(f.relative_to(runtime))));cmd=shlex.split(row['command']);obj=out/(f.stem+'-android.o');cmd[cmd.index('-o')+1]=str(obj);run([*cmd,'-ivfsoverlay',overlay],f.stem+'-android.log',row['directory']);android[f.name]={'passed':True,'object_sha256':sha(obj)}
 assert pins=={str(x.relative_to(ROOT)):sha(x)for x in paths},'Source changed during checks'
 report={'state':'passed','host_tests':reports,'spirv_variants':len(shader_files),'actual_original_variants':len(a.original),'original_resource_sha256':[{'name':x[4],'sha256':sha(x[0]),'offset':int(x[1],0),'bytes':int(x[2].split('+')[0],0),'constants':int(x[2].split('+')[1],0)if '+' in x[2] else 0,'uint_mask':int(x[3],0)}for x in a.original],'android_production_compile':android,'source_sha256':pins,'scope':'Actual translator and program key/memo bodies, synthetic all-slot/type fixtures and optional original resources. Combined D/S comparison retains legacy float/depth behavior; accuracy of integer+Dref is unqualified. No GPU or visual correctness claim.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
