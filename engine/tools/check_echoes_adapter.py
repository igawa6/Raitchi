#!/usr/bin/env python3
"""Validate original-native title policy and actual shared SDK startup bodies; no games/device."""
import argparse,hashlib,json,re,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def function(source,name):
 match=re.search(r'(?m)^(?:static )?(?:void|uint64_t)\s+\*?'+re.escape(name)+r'\s*\([^;]*?\)\s*\{',source)
 if not match:raise RuntimeError('Function not found: '+name)
 at=source.index('{',match.start());depth=1;end=at+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[match.start():end]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018');a=p.parse_args();out=ROOT/'build/echoes-adapter';out.mkdir(parents=True,exist_ok=True);runtime=ROOT/'ports/shared-android/runtime'
 main_source=(runtime/'main.c').read_text();kernel=(runtime/'kernel.c').read_text();fragments=[function(main_source,n) for n in ('run_init_stage','init_sdk','init_main','fini_main')]+[function(kernel,n) for n in ('kernel_run_main','kernel_run_main5')]
 start=main_source.index('    uint32_t argument_count = title_adapter_selected()->startup.argument_count;');end=main_source.index('    (void)ui_run();',start);fragments.append('static void actual_main_startup(uint32_t stack_size,int priority){\n'+main_source[start:end]+'\n}')
 (out/'echoes_startup_fragments.h').write_text('/* Verbatim production startup bodies with explicit host-service stand-ins. */\n'+'\n\n'.join(fragments)+'\n')
 sources=[ROOT/x for x in ('native/module_abi.c','native/runtime/native_manifest.c','native/runtime/session_layout.c','native/adapters/title_adapter.c','native/adapters/las/profile.c','native/adapters/letsgo/profile.c','native/adapters/echoes/profile.c', 'native/adapters/wonder/profile.c')]
 paths=sources+[ROOT/'native/adapters/title_adapter.h',ROOT/'native/runtime/native_manifest.h',ROOT/'native/runtime/aot_cpu.h',runtime/'main.c',runtime/'kernel.c',runtime/'kernel.h',ROOT/'tests/echoes_adapter_test.c',ROOT/'tests/title_adapter_test.c',Path(__file__).resolve()];pins={str(p.relative_to(ROOT)):sha(p) for p in paths}
 def run(args,log):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=30);(out/log).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(log+': '+r.stdout+r.stderr)
  return r.stdout
 common=['-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I',ROOT/'native','-I',out];reports={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  for name,test,cases in [('native',ROOT/'tests/echoes_adapter_test.c',('las','eevee','echoes','wonder')),('existing',ROOT/'tests/title_adapter_test.c',('las','eevee','concurrent'))]:
   binary=out/(name+'-'+mode);run([shutil.which('clang'),*common,*flags,*sources,test,'-pthread','-o',binary],name+'-'+mode+'-build.log')
   for title in cases:
    stdout=run([binary,title],name+'-'+mode+'-'+title+'.log');reports[name+'-'+mode+'-'+title]={'passed':True,'stdout':stdout.strip()}
 target=a.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang';binary=out/'echoes-adapter-android'
 run([target,*common,'-O2','-ffixed-x18','-fstack-protector-strong',*sources,ROOT/'tests/echoes_adapter_test.c','-pthread','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',binary],'android-link.log')
 assert pins=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Source changed during verification'
 result={'schema':1,'state':'passed','host_tests':reports,'android_cross_link_passed':True,'source_sha256':pins,'startup_fragments_sha256':sha(out/'echoes_startup_fragments.h'),'android_test_sha256':sha(binary),'scope':'synthetic native manifests, actual extracted main constructor/SDK startup and kernel main-register functions with explicit service/thread-start stand-ins; no original code, real thread or device/game execution'}
 (out/'verification.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
