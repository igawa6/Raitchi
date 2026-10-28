#!/usr/bin/env python3
"""Asset-free final-flow/resource metadata and captured-draw identity join checks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'ports/shared-android/runtime/nvn/maxwell/translate.c'
HEADER=SOURCE.with_name('shader.h')
EXEC=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c'
TEST=ROOT/'tests/shader_metadata_test.c'

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--android',action='store_true')
    parser.add_argument('--baseline-dir',type=Path,help='Optional pre-change translator/header copies; compare emitted sources byte for byte')
    args=parser.parse_args()
    out=ROOT/'build/shader-metadata';out.mkdir(parents=True,exist_ok=True)
    text=EXEC.read_text();a=text.index('static unsigned pg_log_shader_metadata(');b=text.index('\nstatic void draw_(CmdRec *r, bool indexed) {',a)
    (out/'production_draw_metadata.inc').write_text(text[a:b])
    a=text.index('    if (pg_state == 1 && pg_draws_on() && pg_n > 0) {',b);b=text.index('\n    dl_note("  draw vs',a)
    (out/'production_draw_join.inc').write_text(text[a:b])
    join=text[a:b]
    assert 'st.program->stages[0].code_addr' in join and 'st.program->stages[1].code_addr' in join
    assert 'pr->code[' not in join and 'shader_stage_hash(' not in join
    assert 'pg_verts += vc, pg_vc[pg_n - 1] = vc;' in join
    pins={str(p.relative_to(ROOT)):sha(p)for p in (SOURCE,HEADER,EXEC,TEST,Path(__file__))}
    def run(cmd,name,env=None):
        p=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,env=env,timeout=90)
        (out/name).write_text(p.stdout+p.stderr)
        if p.returncode:raise RuntimeError(name+': '+p.stdout+p.stderr)
        return p.stdout
    modes={'nested':({},1),'flag':({'LARECOMP_VK_NESTED_FLOW':'0'},2),
           'pc':({'LARECOMP_VK_NESTED_FLOW':'0','LARECOMP_SHADER_PC_MACHINE':'1'},3),
           'dynamic':({'LARECOMP_VK_NESTED_FLOW':'0','LARECOMP_SHADER_PC_MACHINE':'1','LARECOMP_DYN_STACKS':'1'},3)}
    configurations=[('optimized',SOURCE,HEADER.parent,['-O2']),('sanitized',SOURCE,HEADER.parent,['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'])]
    if args.baseline_dir:
        baseline=args.baseline_dir.resolve()
        configurations.append(('baseline',baseline/'translate.c',baseline,['-O2','-DMETADATA_BASELINE']))
    results={};spirv=0
    for variant,source,include,flags in configurations:
        binary=out/(variant+'-test')
        run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-missing-field-initializers',*flags,'-I',ROOT/'native','-I',include,'-I',SOURCE.parent,'-I',out,TEST,source,'-lm','-o',binary],variant+'-build.log')
        results[variant]={}
        for mode,(extra,expected)in modes.items():
            for native in (0,1):
                name=f'{variant}-{mode}-{native}';directory=out/name;directory.mkdir(exist_ok=True)
                env={k:v for k,v in os.environ.items()if not k.startswith('LARECOMP_')};env.update(extra)
                results[variant][f'{mode}-{native}']=json.loads(run([binary,directory,native,expected],name+'-run.log',env))
                if variant=='optimized':
                    for shader in sorted(directory.glob('*.frag')):
                        spv=shader.with_suffix('.spv');run(['glslangValidator','-V','--target-env','vulkan1.2',shader,'-o',spv],shader.parent.name+'-'+shader.stem+'-glsl.log')
                        run(['spirv-val','--target-env','vulkan1.2',spv],shader.parent.name+'-'+shader.stem+'-spirv.log');spirv+=1
    unchanged=0
    if args.baseline_dir:
        for path in sorted(out.glob('baseline-*/*.frag')):
            current=out/path.parent.name.replace('baseline-','optimized-',1)/path.name
            assert path.read_bytes()==current.read_bytes(),f'emission changed: {path.name}/{path.parent.name}'
            unchanged+=1
    android=[]
    if args.android:
        rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text())
        for actual in (SOURCE,EXEC):
            suffix='/runtime/'+str(actual.relative_to(ROOT/'ports/shared-android/runtime'))
            row=next(r for r in rows if r['file'].endswith(suffix))
            command=shlex.split(row['command']);obj=out/(actual.stem+'-android.o')
            command[command.index('-o')+1]=str(obj)
            staged_header=ROOT/'work/shared-android/runtime/nvn/maxwell/shader.h'
            overlay=out/(actual.stem+'-overlay.json')
            overlay.write_text(json.dumps({'version':0,'roots':[
                {'type':'file','name':row['file'],'external-contents':str(actual)},
                {'type':'file','name':str(staged_header),'external-contents':str(HEADER)}]}))
            result=subprocess.run(command+['-ivfsoverlay',str(overlay)],cwd=row['directory'],capture_output=True,text=True,timeout=90)
            (out/(actual.stem+'-android.log')).write_text(result.stdout+result.stderr)
            if result.returncode:raise RuntimeError(actual.name+' Android compile: '+result.stdout+result.stderr)
            android.append({'source':str(actual.relative_to(ROOT)),'passed':True,'object_sha256':sha(obj)})
    assert pins=={p:sha(ROOT/p)for p in pins}
    report={'state':'passed','source_sha256':pins,'results':results,'spirv_shaders':spirv,'byte_identical_before_after':unchanged,
            'android':android,'scope':'Actual translator with synthetic straight/nested/flag/PC/native-half/local fixtures; verbatim production capture metadata formatter/join with controlled ready program/pipeline bindings. No device/GPU/game shader payload.'}
    (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
