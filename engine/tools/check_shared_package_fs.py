#!/usr/bin/env python3
"""Actual FS base-image branch tests with real folder layout and controlled package failures."""
import argparse,hashlib,json,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=pathlib.Path,default=pathlib.Path.home()/'android-sdk/ndk/27.2.12479018');a=p.parse_args();out=ROOT/'build/shared-package-fs';out.mkdir(parents=True,exist_ok=True)
 source=ROOT/'ports/shared-android/runtime/services/fs.c';test=ROOT/'tests/shared_package_fs_test.c';runtime=ROOT/'work/shared-android/runtime';builder=runtime/'romfs_build.c';s=source.read_text();fragment=s[s.index('static RomfsLayout g_base;'):s.index('// ------------------------------------------------------------------ RomFS file index')];(out/'shared_package_fs_fragments.h').write_text(fragment)
 (out/'shared_package_file_fragment.h').write_text(s[s.index('bool romfs_package_file('):s.index('// Virtual RomFS image read:')])
 paths=[source,test,builder,runtime/'romfs_build.h',pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x) for x in paths}
 def run(args,name):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  cc=shutil.which('clang');obj=out/(name+'-layout.o');run([cc,'-D_GNU_SOURCE','-std=c11',*flags,'-Dromfs_layout=actual_romfs_layout','-Dromfs_layout_free=actual_romfs_layout_free','-I',runtime,'-c',builder,'-o',obj],name+'-layout-build.log');exe=out/name;run([cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,'-I',runtime,test,obj,'-pthread','-o',exe],name+'-build.log');reports[name]=json.loads(run([exe],name+'-run.log'))
 cc=a.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang';obj=out/'android-layout.o';run([cc,'-std=c11','-O2','-Dromfs_layout=actual_romfs_layout','-Dromfs_layout_free=actual_romfs_layout_free','-I',runtime,'-c',builder,'-o',obj],'android-layout-build.log');target=out/'shared-package-fs-test';run([cc,'-std=c11','-O2','-ffixed-x18','-Wall','-Wextra','-Werror','-I',out,'-I',runtime,test,obj,'-pthread','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',target],'android-link.log')
 assert pins=={k:sha(ROOT/k) for k in pins},'Source changed during verification'
 report={'state':'passed','host_tests':reports,'android_cross_link_passed':True,'android_test_sha256':sha(target),'source_sha256':pins,'fragment_sha256':sha(out/'shared_package_fs_fragments.h'),'scope':'Actual services/fs.c base-image bodies and real RomFS folder layout, mocked package open/size/read/close and guest commit; no IPC/save/HLE changes or device execution.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
