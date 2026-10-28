#!/usr/bin/env python3
"""Execute actual float/UINT Vulkan sampler helpers; compile full Android texture TU."""
import hashlib,json,shlex,shutil,subprocess
from pathlib import Path
from check_storage_image_views import function
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    out=ROOT/'build/uint-sampler';out.mkdir(parents=True,exist_ok=True)
    source=ROOT/'ports/shared-android/runtime/nvn/vulkan/textures.c';header=source.with_name('vk.h');nvn=source.parent.parent/'nvn.h';test=ROOT/'tests/uint_sampler_test.c'
    paths=[source,header,nvn,test,Path(__file__),ROOT/'tools/check_storage_image_views.py'];pins={str(p.relative_to(ROOT)):sha(p)for p in paths}
    text=source.read_text();types=nvn.read_text();end=types.index('} NvnSamplerDesc;')+len('} NvnSamplerDesc;');start=types.rindex('typedef struct {',0,end)
    (out/'production_types.inc').write_text(types[start:end]+'\n')
    start=text.index('#define UINT_SAMPLER_CAP ');end=text.index('VkSampler vk_default_sampler',start)
    helpers=function(text,'compare_op')+'\n'+function(text,'vk_sampler')+'\n'+text[start:end]
    (out/'production_samplers.inc').write_text(helpers)
    def run(cmd,log):
        p=subprocess.run(list(map(str,cmd)),cwd=ROOT,capture_output=True,text=True,timeout=90);(out/log).write_text(p.stdout+p.stderr)
        if p.returncode:raise RuntimeError(log+': '+p.stdout+p.stderr)
        return p.stdout.strip()
    reports={};inc=ROOT/'work/las-baseline/build/android/deps/include'
    for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        binary=out/mode;run([shutil.which('clang'),'-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',*flags,'-I',out,'-I',inc,test,'-lm','-o',binary],mode+'-build.log')
        reports[mode]=json.loads(run([binary],mode+'-run.log'));assert reports[mode]['state']=='passed'
    rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/textures.c'))
    overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':row['file'],'external-contents':str(source)},{'type':'file','name':str(Path(row['file']).with_name('vk.h')),'external-contents':str(header)}]}))
    cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(out/'textures-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd,'android-full-tu.log')
    assert pins=={str(p.relative_to(ROOT)):sha(p)for p in paths},'Source changed during checks'
    report={'schema':1,'scope':'Actual production vk_sampler/vk_sampler_uint with exact NvnSamplerDesc declaration and controlled vkCreateSampler. Full production Android textures TU compile. No GPU/device qualification.','tests':reports,'android_full_tu':True,'source_sha256':pins}
    (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
