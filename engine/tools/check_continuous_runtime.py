#!/usr/bin/env python3
"""Actual continuous orchestration C with mock exits, plus actual registry API tests."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]
CASES=('sentinel','host','nested','exit-thread','virtual','original','hooks','literals','stop',
       'configure-failure','invalid-event','missing-event','unbound-host','invalid-hook')
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018');args=parser.parse_args()
    out=ROOT/'build/continuous-runtime-test';out.mkdir(parents=True,exist_ok=True)
    # rt.h needs only this platform declaration in the mock-linked unit.
    (out/'platform.h').write_text('#pragma once\n#include <stdint.h>\nuint64_t plat_now_ns(void);\n')
    runtime=ROOT/'ports/shared-android/runtime';test=ROOT/'tests/continuous_runtime_test.c';source=runtime/'continuous_runtime.c';control=ROOT/'native/native-game-backend/control_step.c'
    registry=[ROOT/'native/module_abi.c',ROOT/'native/runtime/aot_dispatch.c',ROOT/'native/runtime/native_manifest.c',ROOT/'native/runtime/aot_hooks.c']
    queue=runtime/'nvn/rqueue.c';dispatch=runtime/'dispatch.c'
    def code_only(text):return re.sub(r'/\*.*?\*/|//[^\n]*','',text,flags=re.S)
    assert re.search(r'\bcrash_handler_install\s*\(\s*\)\s*;',code_only(queue.read_text())),'Renderer worker must use continuous-aware installer'
    raw_callers=[]
    for unit in runtime.rglob('*.c'):
        if re.search(r'\bplat_crash_handler_install\s*\(',code_only(unit.read_text())):
            assert unit==dispatch,'Raw process-wide signal installer bypass: '+str(unit)
            raw_callers.append(str(unit.relative_to(ROOT)))
    assert raw_callers==[str(dispatch.relative_to(ROOT))]
    signal_installer={'renderer_worker_uses_wrapper':True,'raw_installer_callers':raw_callers}
    paths=[source,source.with_suffix('.h'),runtime/'continuous_context.h',runtime/'continuous_image.h',runtime/'continuous_signals.h',runtime/'native_hooks.h',runtime/'rt.h',runtime/'cpu.h',runtime/'kernel.h',queue,dispatch,control,control.with_suffix('.h'),test,*registry,ROOT/'native/runtime/aot_hooks.h',ROOT/'native/runtime/aot_dispatch.h',ROOT/'native/runtime/aot_cpu.h',Path(__file__).resolve()]
    pinned={str(p.relative_to(ROOT)):sha(p) for p in paths}
    def run(command,log):
        result=subprocess.run(list(map(str,command)),text=True,capture_output=True,timeout=60);(out/log).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(log+': '+result.stdout+result.stderr)
        return result.stdout
    cc=shutil.which('clang');assert cc;common=['-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I',ROOT/'native','-I',runtime,'-I',out]
    reports={}
    for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        executable=out/mode;run([cc,*common,*flags,source,control,test,'-pthread','-o',executable],mode+'-build.log');reports[mode]={}
        for case in CASES:
            report=json.loads(run([executable,case],mode+'-'+case+'.log').splitlines()[-1]);assert report['state']=='passed';reports[mode][case]=report
        executable=out/(mode+'-registry');run([cc,*common,*flags,'-DCNCE_REGISTRY_ENUM_TEST',*registry,test,'-pthread','-o',executable],mode+'-registry-build.log');reports[mode]['registry']=json.loads(run([executable],mode+'-registry-run.log'));assert reports[mode]['registry']['state']=='passed'
    android=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang';target=out/'android-runtime-test';registry_target=out/'android-registry-test'
    run([android,*common,'-O2','-ffixed-x18',source,control,test,'-pthread','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',target],'android-runtime-link.log')
    run([android,*common,'-O2','-ffixed-x18','-DCNCE_REGISTRY_ENUM_TEST',*registry,test,'-pthread','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',registry_target],'android-registry-link.log')
    assert pinned=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Sources changed during verification; rerun after coordinator freeze'
    record={'schema':1,'host_tests':reports,'signal_installer_regression':signal_installer,'android_cross_link_passed':True,'sources_sha256':pinned,'android_artifacts_sha256':{p.name:sha(p) for p in (target,registry_target)},'scope':'Actual continuous_runtime/control_step C with mocked gateway/image/signals/services; actual registry enumeration implementations. No hardware or actual guest execution.'}
    (out/'verification.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
if __name__=='__main__':main()
