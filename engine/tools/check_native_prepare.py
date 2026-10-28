#!/usr/bin/env python3
"""Validate actual portable native preparation and JNI cross-compilation; no assets/device."""
import os
import hashlib,importlib.util,json,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-prepare-runtime'
SOURCES=['native/runtime/native_prepare.c','native/module_abi.c','native/runtime/native_manifest.c','native/runtime/nso_image.c','native/runtime/session_layout.c','native/adapters/title_adapter.c','native/adapters/las/profile.c','native/adapters/letsgo/profile.c','native/adapters/echoes/profile.c', 'native/adapters/wonder/profile.c','native/adapters/generic/generic.c']
def run(cmd):
 p=subprocess.run(list(map(str,cmd)),text=True,capture_output=True,timeout=90)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);cc=shutil.which('clang');cxx=shutil.which('clang++');assert cc and cxx
 spec=importlib.util.spec_from_file_location('prepare',ROOT/'tools/prepare_native_game.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);contract=mod.contract_digest().hex();results={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  objs=[]
  for index,p in enumerate([*SOURCES,'tests/native_prepare_test.c']):
   obj=OUT/f'{name}-{index}.o';run([cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',ROOT/'native','-c',ROOT/p,'-o',obj]);objs.append(obj)
  obj=OUT/f'{name}-hash.o';run([cxx,'-std=c++17',*flags,'-I',ROOT/'native','-I',ROOT/'sources/las-android/runtime','-c',ROOT/'ports/shared-android/runtime/shared_hash.cpp','-o',obj]);objs.append(obj)
  binary=OUT/name;run([cxx,*flags,*objs,'-pthread','-o',binary])
  with tempfile.TemporaryDirectory(prefix='prepare-',dir=OUT) as tmp:
   results[name]=run([binary,tmp,contract]);print(name+': '+results[name],flush=True)
   # Compare the C wire builder byte-for-byte against the established public Python/C validation flow.
   for title in ['eevee','las']:
    root=Path(tmp)/(title+'-1');expected=root/'reference.manifest';mod.prepare(title,root/'exefs',expected)
    assert expected.read_bytes()==(root/'native.manifest').read_bytes(),title+' public preparation wire mismatch'
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/('runtime-'+name);run([cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',ROOT/'native','-I',ROOT/'ports/shared-android/runtime',ROOT/'tests/native_package_runtime_test.c',ROOT/'ports/shared-android/runtime/native_package_android.c',*(ROOT/p for p in SOURCES[1:]),'-pthread','-o',exe]);results['runtime_'+name]=run([exe]);print(results['runtime_'+name],flush=True)
 ndk=Path(os.environ.get('ANDROID_SDK_ROOT',Path.home()/'android-sdk'))/'ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang'
 run([ndk,'-std=c11','-Wall','-Wextra','-Werror','-O2','-I',ROOT/'native','-c',ROOT/'native/runtime/native_prepare.c','-o',OUT/'prepare.android.o'])
 for source,name,extra in [('native_package_prepare_android.c','jni', ['-I',ROOT/'build/shared-android/include']),('native_package_android.c','package',['-I',ROOT/'work/las-baseline/build/android/SDL3/include'])]:
  run([ndk,'-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-O2','-I',ROOT/'native',*extra,'-c',ROOT/'ports/shared-android/runtime'/source,'-o',OUT/(name+'.android.o')])
 paths=[*(ROOT/p for p in SOURCES),ROOT/'native/runtime/native_prepare.h',ROOT/'tests/native_prepare_test.c',ROOT/'tests/native_package_runtime_test.c',*(ROOT/'ports/shared-android/runtime'/p for p in ['native_package_prepare_android.c','native_package_android.c','native_package_android.h']),Path(__file__)];report={'schema':1,'scope':'Actual portable preparer+real SHA256+actual adapters/NSO/LZ4/native manifest/layout; synthetic original files and filesystem failure checks; byte-identical established public preparation flow. Runtime bridge policy uses controlled provider stand-ins. No package decryption, SAF, GPU, device or game execution.','results':results,'android_prepare_tu':True,'android_jni_and_runtime_bridge_tus':True,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
