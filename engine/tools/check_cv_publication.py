#!/usr/bin/env python3
"""Deterministic actual-kernel lost-notification regression, no game/device work."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def function(text,name):
    match=re.search(r'(?:static )?(?:bool|void|uint32_t)\s+'+re.escape(name)+r'\(',text)
    if not match:raise RuntimeError('Missing production function: '+name)
    start=match.start();at=text.index('{',start);depth=1;end=at+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--android',action='store_true',help='Also compile actual production kernel TU')
    parser.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018')
    args=parser.parse_args();out=ROOT/'build/cv-publication';out.mkdir(parents=True,exist_ok=True)
    source=ROOT/'ports/shared-android/runtime/kernel.c';test=ROOT/'tests/cv_publication_test.c'
    text=source.read_text();wait=function(text,'svc_wait_process_wide_key')
    publication='    __atomic_store_n(u32p(key), 1, __ATOMIC_SEQ_CST);'
    assert wait.count(publication)==1 and wait.index(publication)<wait.index('mutex_release(mutex);')
    # Move ONLY the flag store back to its previously broken position. The
    # production fixed body and mutant share every other executable statement.
    old=wait.replace(publication+'\n','',1)
    anchor='    t->wait_tag = tag;'
    assert old.count(anchor)==1
    old=old.replace(anchor,anchor+'\n'+publication,1)
    names=('deadline','kwait','mutex_acquire_or_mark','mutex_release','waitlist_add',
           'waitlist_remove','block_current','svc_signal_process_wide_key')
    parts=[function(text,name) for name in names]
    parts[3]=parts[3].replace('mutex_release(','release_body(',1)
    fragments='/* Verbatim MIT production functions; only names separate variants. */\n'+'\n\n'.join(parts)+'\n'
    fragments+=wait.replace('svc_wait_process_wide_key(','wait_publish_first(',1)+'\n'
    fragments+=old.replace('svc_wait_process_wide_key(','wait_old_mutant(',1)+'\n'
    (out/'cv_publication_fragments.h').write_text(fragments)
    paths=[source,test,Path(__file__).resolve()];pins={str(p.relative_to(ROOT)):sha(p) for p in paths}

    def run(command,log,cwd=None):
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=60,cwd=cwd)
        (out/log).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(log+': '+result.stdout+result.stderr)
        return result.stdout

    compiler=shutil.which('clang');assert compiler
    common=['-std=c11','-Wall','-Wextra','-Werror','-I',out]
    reports={}
    for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        binary=out/mode;run([compiler,*common,*flags,test,'-pthread','-o',binary],mode+'-build.log')
        report=json.loads(run([binary],mode+'-run.log'))
        assert report['state']=='passed' and report['original_lost_notifications']==20 and report['publish_first_successful_wakes']==20
        reports[mode]=report
    toolchain=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin';binary=out/'cv-publication-android'
    run([toolchain/'aarch64-linux-android30-clang',*common,'-O2','-ffixed-x18',test,'-pthread',
         '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',binary],'android-test-link.log')
    production={}
    if args.android:
        stage=ROOT/'work/shared-android/runtime/kernel.c'
        rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text())
        row=next(x for x in rows if x['file'].endswith('/runtime/kernel.c'))
        command=shlex.split(row['command']);command[command.index('-o')+1]=str(out/'kernel-android.o')
        overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(stage),'external-contents':str(source)}]}))
        run([*command,'-ivfsoverlay',overlay],'android-production-compile.log',cwd=row['directory'])
        production={'passed':True,'object_sha256':sha(out/'kernel-android.o')}
    assert pins=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Production source changed during checks'
    receipt={'state':'passed','host_tests':reports,'android_cross_link_passed':True,'android_production_compile':production,
        'source_sha256':pins,'fragments_sha256':sha(out/'cv_publication_fragments.h'),
        'ordering':'Existing seq-cst key store precedes existing seq-cst guest mutex release; producer acquire observes publication. No additional DMB needed for these C atomics.',
        'primary_reference':'https://raw.githubusercontent.com/Atmosphere-NX/Atmosphere/master/libraries/libmesosphere/source/kern_k_condition_variable.cpp',
        'reference_lines':'230-240: publish waiter key, DMB, then write mutex next-owner value',
        'scope':'Actual production wait/signal/mutex/block_current/kwait with minimal KThread and controlled post-unlock hook. Old-order mutant alone moves publication. No device or game execution; does not establish sole cause of observed game hang.'}
    (out/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))


if __name__=='__main__':main()
