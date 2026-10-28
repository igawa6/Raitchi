#!/usr/bin/env python3
"""Verify continuous image ownership/W^X/cache semantics with synthetic modules."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018')
    parser.add_argument('--out',type=Path,default=ROOT/'build/continuous-image')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    cc=shutil.which('clang');assert cc
    source=ROOT/'ports/shared-android/runtime/continuous_image.c';header=source.with_suffix('.h')
    patch=ROOT/'native/continuous-nce/patch_plan.c';test=ROOT/'tests/continuous_image_test.c'
    paths=[source,header,patch,patch.with_suffix('.h'),test,ROOT/'native/runtime/aot_cpu.h',ROOT/'native/runtime/session_layout.h',Path(__file__).resolve()]
    pinned={str(p.relative_to(ROOT)):sha(p) for p in paths}
    def run(command,log):
        result=subprocess.run(list(map(str,command)),text=True,capture_output=True,timeout=60)
        (out/log).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(log+': '+result.stdout+result.stderr)
        return result.stdout
    common=['-std=c11','-Wall','-Wextra','-Werror','-DCNCE_IMAGE_WRAP_PROTECTION','-Wl,--wrap=mprotect','-I',ROOT/'native','-I',source.parent]
    reports={}
    for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        executable=out/name
        run([cc,*common,*flags,patch,source,test,'-pthread','-o',executable],name+'-build.log')
        report=json.loads(run([executable],name+'-run.log'));assert report['state']=='passed' and report['checks']>=8200;reports[name]=report
    android=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang';target=out/'continuous-image-test'
    run([android,*common,'-O2','-ffixed-x18',patch,source,test,'-pthread','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',target],'android-link.log')
    assert pinned=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Source changed during verification'
    report={'schema':1,'host_tests':reports,'android_cross_link_passed':True,'source_sha256':pinned,'android_test_sha256':sha(target),'scope':'Synthetic owned reservations: before-image 16MiB bank, exact signed B26 boundaries, occupied/outside/too-far rejection without guest writes/protection changes, rollback; two fixed modules, all4096fakeHLE, protected metadata, dedicated cache pages, phase-aware PC mapping and cleanup; no guest/device execution'}
    (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
