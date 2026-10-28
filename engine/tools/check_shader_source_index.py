#!/usr/bin/env python3
"""Asset-free bounded RomFS enumeration and BNSH discovery regression gates."""
import argparse,hashlib,json,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=pathlib.Path,default=pathlib.Path.home()/'android-sdk/ndk/27.2.12479018');a=p.parse_args();out=ROOT/'build/shader-source-index';out.mkdir(parents=True,exist_ok=True)
 paths=[ROOT/x for x in ('ports/shared-android/runtime/nxpkg.c','ports/shared-android/runtime/nxpkg.h','ports/shared-android/runtime/nvn/shader_source_index.c','ports/shared-android/runtime/nvn/shader_source_index.h','tests/shader_source_index_test.c','tools/check_shader_source_index.py')];pins={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in paths}
 def run(cmd,name):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=120);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,ROOT/'tests/shader_source_index_test.c','-pthread','-o',exe],name+'-build.log');reports[name]=json.loads(run([exe],name+'-run.log'))
 target=out/'shader-source-index-android';run([a.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-ffixed-x18',ROOT/'tests/shader_source_index_test.c','-pthread','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',target],'android-link.log')
 assert pins=={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in paths},'sources changed during test'
 report={'state':'passed','tests':reports,'android_link':'passed','source_sha256':pins,'scope':'Synthetic source bytes only; no package scan or device/game qualification'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
