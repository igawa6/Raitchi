#!/usr/bin/env python3
"""Asset-free native LAS compatibility and actual pinned frame-governor tests."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]
CASES=('semantics','eevee','aot','bad-opcode','bad-target')
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018');args=parser.parse_args();out=ROOT/'build/native-las-patches';out.mkdir(parents=True,exist_ok=True)
    runtime=ROOT/'ports/shared-android/runtime';upstream=ROOT/'sources/las-android/runtime';source=runtime/'native_las_patches.c';hooks=runtime/'native_hooks.c';test=ROOT/'tests/native_las_patches_test.c';framepace=upstream/'hle/framepace.c'
    stage=out/'runtime';(stage/'hle').mkdir(parents=True,exist_ok=True)
    shutil.copyfile(framepace,stage/'hle/framepace.c')
    for name in ('hle.h','settings.h','cosmetic_colour.h'):shutil.copyfile(upstream/name,stage/name)
    (stage/'platform.h').write_text('#pragma once\n#include <stdint.h>\nuint64_t plat_now_ns(void);\n')
    paths=[source,source.with_suffix('.h'),hooks,hooks.with_suffix('.h'),test,framepace,upstream/'hle.h',upstream/'settings.h',upstream/'cosmetic_colour.h',upstream/'game_addresses.h',upstream/'game_addresses/patches_v1_0_0.h',upstream/'game_addresses/runtime_v1_0_0.h',runtime/'cpu.h',runtime/'rt.h',ROOT/'native/runtime/aot_hooks.h',Path(__file__).resolve()]
    pinned={str(p.relative_to(ROOT)):sha(p) for p in paths}
    def run(command,log):
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=60);(out/log).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(log+': '+result.stdout+result.stderr)
        return result.stdout
    common=['-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I',ROOT/'native','-I',runtime,'-I',stage,'-I',upstream]
    cc=shutil.which('clang');assert cc;reports={}
    for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        frame_object=out/(mode+'-framepace.o');run([cc,*common,*flags,'-Wno-format','-c',stage/'hle/framepace.c','-o',frame_object],mode+'-framepace-build.log')
        executable=out/mode;run([cc,*common,*flags,source,hooks,test,frame_object,'-lm','-o',executable],mode+'-build.log');reports[mode]={}
        for case in CASES:
            result=json.loads(run([executable,case],mode+'-'+case+'.log'));assert result['state']=='passed';reports[mode][case]=result
    android=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang';frame_object=out/'android-framepace.o';target=out/'native-las-patches-test'
    run([android,*common,'-O2','-ffixed-x18','-Wno-format','-c',stage/'hle/framepace.c','-o',frame_object],'android-framepace-build.log')
    run([android,*common,'-O2','-ffixed-x18',source,hooks,test,frame_object,'-lm','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',target],'android-link.log')
    assert pinned=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Sources changed during verification'
    record={'schema':1,'host_tests':reports,'android_cross_link_passed':True,'sources_sha256':pinned,'android_test_sha256':sha(target),'scope':'Actual native helper+native_hooks C and unchanged pinned framepace source, synthetic GameAddrs/Cpu/object buffers; other graphics helpers stubbed to check exact outputs; no game/device execution'}
    (out/'verification.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
if __name__=='__main__':main()
