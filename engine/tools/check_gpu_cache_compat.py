#!/usr/bin/env python3
"""Exercise production GPU metadata IO and real-compiler dependency fingerprints."""
import hashlib,json,pathlib,shutil,subprocess,tempfile
import gpu_cache_fingerprint as fp
ROOT=pathlib.Path(__file__).resolve().parents[1]
OUT=ROOT/'build/gpu-cache-compat'
def run(args,cwd=None):
 p=subprocess.run(list(map(str,args)),cwd=cwd,capture_output=True,text=True,timeout=60)
 if p.returncode:raise RuntimeError(str(args)+'\n'+p.stdout+p.stderr)
 return p.stdout.strip()
def fingerprint_tests():
 with tempfile.TemporaryDirectory(prefix='gpu-cache-closure-') as directory:
  base=pathlib.Path(directory);src=base/'source';gen=base/'generated';(src/'runtime/nvn').mkdir(parents=True);gen.mkdir()
  shader=src/'runtime/nvn/shaders.c';backend=src/'runtime/nvn/exec.c';header=src/'runtime/nvn/dependency.h';transitive=src/'runtime/nvn/nested.h'
  header.write_text('#include "nested.h"\n');transitive.write_text('#define VALUE 1\n')
  shader.write_text('#include "dependency.h"\n#include "gpu_cache_build.h"\nint f(void){return VALUE;}\n');backend.write_text('#include "dependency.h"\nint g(void){return VALUE;}\n')
  archive=base/'shaderc.a';archive.write_bytes(b'compiler archive fixture');toolchain=base/'toolchain.conf';toolchain.write_text('toolchain version 1')
  compiler=base/'compiler';compiler.write_text('#!/bin/sh\nexec '+shutil.which('clang')+' "$@"\n');compiler.chmod(0o700)
  rows=[{'file':str(p),'directory':str(src),'arguments':None,'command':' '.join([str(compiler),'-I'+str(gen),'-O2','-c',str(p),'-o',str(base/(p.stem+'.o'))])} for p in (shader,backend)]
  direct_sources=[]
  for name in ('translate.c','direct_spirv.c','shader_compiler.c'):
   p=src/'runtime/nvn/maxwell'/name;p.parent.mkdir(exist_ok=True)
   p.write_text('#include "../dependency.h"\nint '+name.replace('.c','')+'(void){return VALUE;}\n')
   direct_sources.append(p)
   rows.append({'file':str(p),'directory':str(src),'arguments':None,'command':' '.join([str(compiler),'-I'+str(gen),'-O2','-c',str(p),'-o',str(base/(p.stem+'.o'))])})
  commands=base/'commands.json';commands.write_text(json.dumps(rows));fp.bootstrap(gen/'gpu_cache_build.h')
  def build():return fp.build(commands,src,ROOT,gen,archive,[toolchain])
  original=build();checks=[]
  for p in direct_sources:
   before=p.read_bytes();p.write_bytes(before+b'\n/* compiler implementation changed */\n')
   changed=build();assert changed['compiler_id']!=original['compiler_id'] and changed['renderer_id']!=original['renderer_id'];p.write_bytes(before)
   checks.append(p.name+' implementation invalidates compiler and renderer')
  rows[-1]['command']+=' -fno-fast-math';commands.write_text(json.dumps(rows));assert build()['compiler_id']!=original['compiler_id']
  rows[-1]['command']=rows[-1]['command'].removesuffix(' -fno-fast-math');commands.write_text(json.dumps(rows));assert build()==original
  checks.append('direct compiler command flags invalidate compiler identity')
  for relative in ('runtime/kernel.c','runtime/services/audio.c','native/continuous-nce/dispatch.c','runtime/ui/menu.mm'):
   p=src/relative;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('CPU/UI change\n');assert build()['renderer_id']==original['renderer_id'];checks.append(relative+' stable')
  for p in (backend,header,transitive,archive,toolchain,compiler):
   old=p.read_bytes();p.write_bytes(old+b'\n/* changed */\n' if p!=compiler else old+b'# changed\n')
   changed=build();assert changed['renderer_id']!=original['renderer_id'];p.write_bytes(old);checks.append(p.name+' invalidates')
  # Actual flags are hashed; a genuinely included conditional header is scanned.
  conditional=src/'runtime/nvn/conditional.h';conditional.write_text('#define OTHER 1\n')
  shader.write_text(shader.read_text()+'#ifdef ENABLE_EXTRA\n#include "conditional.h"\n#endif\n');baseline=build()
  conditional.write_text('#define OTHER 2\n');assert build()['renderer_id']==baseline['renderer_id']
  rows[0]['command']+=' -DENABLE_EXTRA=1';commands.write_text(json.dumps(rows));enabled=build();assert enabled['renderer_id']!=baseline['renderer_id']
  assert str(conditional.resolve()) in enabled['input_sha256'];conditional.write_text('#define OTHER 3\n');assert build()['renderer_id']!=enabled['renderer_id'];checks.append('actual conditional preprocessor dependencies/flags')
  fp.write(enabled,gen/'gpu_cache_build.h',base/'identity.json');assert fp.build(commands,src,ROOT,gen,archive,[toolchain])['renderer_id']!=enabled['renderer_id']
  # verify must reject changed provenance; READY header itself is excluded to avoid a cycle.
  try:fp.verify(enabled)
  except RuntimeError:pass
  else:raise AssertionError('stale identity accepted')
  conditional.write_text('#define OTHER 2\n');assert build()==enabled;fp.verify(enabled)
  assert (gen/'gpu_cache_build.h').read_text().find(enabled['renderer_id'])>=0;checks.append('generated header cycle excluded; stale receipt fails')
  header.write_text('#include "missing.h"\n')
  try:build()
  except RuntimeError:pass
  else:raise AssertionError('missing actual dependency accepted')
  checks.append('preprocessor error fails closed')
  return checks

def main():
 OUT.mkdir(parents=True,exist_ok=True);results={'dependency_mutations':fingerprint_tests()}
 # Compile actual helper + production SHA with actual public metadata structures.
 include=OUT/'include';include.mkdir(exist_ok=True);fp.bootstrap(include/'gpu_cache_build.h')
 cc=shutil.which('clang');cxx=shutil.which('clang++');runtime=ROOT/'ports/shared-android/runtime'
 for mode in ('bootstrap','ready'):
  (include/'gpu_cache_build.h').write_text('#define GPU_CACHE_BUILD_READY '+('0' if mode=='bootstrap' else '1')+'\n#define GPU_RENDERER_ID_HEX "'+'ab'*32+'"\n#define GPU_COMPILER_ID_HEX "'+'cd'*32+'"\n')
  for variant,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
   objects=[]
   for source in (runtime/'shared_gpu_cache.c',ROOT/'tests/gpu_cache_compat_test.c',runtime/'shared_hash.cpp'):
    obj=OUT/(mode+'-'+variant+'-'+source.stem+'.o');objects.append(obj)
    run([cxx if source.suffix=='.cpp' else cc,'-std=c++17' if source.suffix=='.cpp' else '-std=gnu11','-Wall','-Wextra','-Werror',*flags,*(['-Dmalloc=cache_test_malloc'] if source.name=='shared_gpu_cache.c' else []),'-I'+str(include),'-I'+str(runtime),'-I'+str(ROOT/'native'),'-I'+str(ROOT/'work/shared-android/runtime'),'-c',source,'-o',obj])
   exe=OUT/(mode+'-'+variant);run([cxx,*flags,*objects,'-o',exe])
   with tempfile.TemporaryDirectory(prefix='gpu-cache-io-') as tmp:results[mode+'-'+variant]=run([exe,mode],tmp)
 results['source_sha256']={str(p.relative_to(ROOT)):fp.sha(p) for p in (runtime/'shared_gpu_cache.c',runtime/'shared_gpu_cache.h',ROOT/'tests/gpu_cache_compat_test.c',pathlib.Path(__file__),pathlib.Path(fp.__file__))}
 results['scope']='Real host clang -M dependency scans, actual runtime metadata/header/IO+SHA, immutable manifest doubles; no Vulkan driver/device claims.'
 (OUT/'verification.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
if __name__=='__main__':main()
