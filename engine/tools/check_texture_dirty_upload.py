#!/usr/bin/env python3
"""Reproduce sparse tracked-texture misses against the actual upload decision C."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
from check_storage_image_views import function as production_function, struct as production_struct
ROOT=Path(__file__).resolve().parents[1]
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def function(text,name):
    start=text.index('static uint64_t '+name+'(');body=text.index('{',start);end=body+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
def prefix(text):
    start=text.index('static void upload_texture(NvnTexture *t, TexBackend *b) {');end=text.index('    slow_note_upload(total);',start)+len('    slow_note_upload(total);')
    counters=text[text.index('uint64_t prof_tex_checks,'):start]
    guards=''
    if '// BEGIN explicit CPU texture-write coherence' in text:
        guards='#define TEXTURE_EXPLICIT_CPU_WRITES 1\n'+text[text.index('static bool texture_write_pending('):text.index('static void texture_write_gpu_check(')]
    # Model only the GPU transfer tail as successful. Keep its exact final
    # generation acknowledgement so subsequent same-byte checks remain real.
    acknowledgement=''
    if guards:
        tail=production_function(text[start:],'upload_texture')
        last=tail.rfind('    b->cpu_write_generation = write_generation;')
        assert last>=0
        acknowledgement=tail[last:tail.index('\n',last)+1]
    return (guards+'static uint64_t full_hash_budget,full_hash_budget_frame;\n'+function(text,'hash_full')+'\n'+function(text,'fnv_sample')+'\n'+counters+text[start:end]+'\n'+acknowledgement+'}\n')
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018');args=parser.parse_args();out=ROOT/'build/texture-dirty-upload';out.mkdir(parents=True,exist_ok=True)
    source=ROOT/'ports/shared-android/runtime/nvn/vulkan/textures.c';baseline=ROOT/'sources/las-android/runtime/nvn/vulkan/textures.c';hash_header=ROOT/'sources/las-android/runtime/nvn/tex_hash.h';test=ROOT/'tests/texture_dirty_upload_test.c'
    runtime=ROOT/'ports/shared-android/runtime';nvn=runtime/'nvn';header=source.with_name('vk.h')
    paths=[source,header,nvn/'nvn.h',nvn/'tegra.h',nvn/'tegra.c',nvn/'pool_mapping.h',nvn/'pool_mapping.c',nvn/'objects.c',baseline,hash_header,test,Path(__file__).resolve()];pinned={str(p.relative_to(ROOT)):sha(p) for p in paths}
    (out/'production_backend.inc').write_text('typedef void *VmaAllocation;\n'+production_struct(header.read_text(),'typedef struct TexBackend {','} TexBackend;')+'\n')
    (out/'production_pool_resolver.inc').write_text(production_function((nvn/'objects.c').read_text(),'nvn_pool_resolve_span')+'\n')
    shutil.copyfile(hash_header,out/'tex_hash.h');actual=prefix(source.read_text());old=prefix(baseline.read_text())
    assert 'if (ch > 0) full = true;' in actual and 'if (ch > 0) full = true;' not in old
    (out/'texture_upload_prefix.inc').write_text(actual)
    def run(command,log,expect_failure=False):
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=60);(out/log).write_text(result.stdout+result.stderr)
        if expect_failure:
            assert result.returncode and 'uploads==start_uploads+2' in result.stderr,'Pinned baseline did not reproduce the sparse-miss bug'
        elif result.returncode:raise RuntimeError(log+': '+result.stdout+result.stderr)
        return result.stdout
    cc=shutil.which('clang');assert cc;common=['-std=c11','-Wall','-Wextra','-Werror','-I',out,'-I',ROOT/'native','-I',runtime,'-I',ROOT/'work/shared-android/runtime/nvn','-I',ROOT/'work/las-baseline/build/android/deps/include'];dependencies=[nvn/'tegra.c',nvn/'pool_mapping.c','-pthread'];reports={}
    for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        executable=out/mode;run([cc,*common,*flags,test,*dependencies,'-o',executable],mode+'-build.log');reports[mode]={}
        for fast in (0,1):
            report=json.loads(run([executable,fast],mode+'-fast'+str(fast)+'.log'));assert report['state']=='passed' and report['checks']>=40;reports[mode][str(fast)]=report
    # Compile the exact old prefix against the same regression fixture. Both
    # hash modes must fail at the known sparse-update upload expectation.
    baseline_dir=out/'baseline';baseline_dir.mkdir(exist_ok=True);shutil.copyfile(hash_header,baseline_dir/'tex_hash.h');(baseline_dir/'texture_upload_prefix.inc').write_text(old)
    executable=baseline_dir/'baseline';run([cc,'-I',baseline_dir,*common,'-O2',test,*dependencies,'-o',executable],'baseline-build.log')
    for fast in (0,1):run([executable,fast],'baseline-fast'+str(fast)+'.log',expect_failure=True)
    tools=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin';target=out/'texture-dirty-upload-test';run([tools/'aarch64-linux-android30-clang',*common,'-O2','-ffixed-x18',test,*dependencies,'-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',target],'android-regression-link.log')
    # Compile the complete production translation unit with the existing offline
    # Android build's flags/includes, directing output only into this fixture.
    commands=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());entry=next(item for item in commands if item['file'].endswith('/nvn/vulkan/textures.c'))
    # Redirect only this virtual file, preserving production header identities
    # and relative include resolution without mutating the coordinator's stage.
    overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':entry['file'],'external-contents':str(source)},{'type':'file','name':str(Path(entry['file']).with_name('vk.h')),'external-contents':str(header)},{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/tegra.h'),'external-contents':str(ROOT/'ports/shared-android/runtime/nvn/tegra.h')}]}))
    command=shlex.split(entry['command']);command[command.index('-o')+1]=str(out/'textures.o')
    command[1:1]=['-ivfsoverlay',str(overlay)];run(command,'android-full-translation-unit.log')
    assert pinned=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Sources changed during verification'
    report={'schema':1,'host_tests':reports,'old_sparse_miss_reproduced_in_both_hash_modes':True,'exact_current_structs':True,'real_tegra_layout_and_pool_resolver':True,'gpu_tail':'Controlled successful transfer, then exact production generation acknowledgement; no GPU modeled','android_regression_link_passed':True,'android_full_translation_unit_compile_passed':True,'source_sha256':pinned,'android_artifacts_sha256':{p.name:sha(p) for p in (target,out/'textures.o')},'actual_decision_prefix_sha256':hashlib.sha256(actual.encode()).hexdigest(),'scope':'Actual texture upload decision prefix plus actual fast/legacy hashes, synthetic1MiB/8MiB atlases, no GPU/game/device execution'}
    (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
