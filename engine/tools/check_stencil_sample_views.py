#!/usr/bin/env python3
"""Actual sampling-aspect policy and view cache; controlled Vulkan, no device use."""
import hashlib,json,shlex,shutil,subprocess
from pathlib import Path
from check_storage_image_views import function,struct
ROOT=Path(__file__).resolve().parents[1]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    out=ROOT/'build/stencil-sample-views';out.mkdir(parents=True,exist_ok=True)
    source=ROOT/'ports/shared-android/runtime/nvn/vulkan/textures.c';header=source.with_name('vk.h');nvn=source.parent.parent/'nvn.h';test=ROOT/'tests/stencil_sample_view_test.c'
    paths=[source,header,nvn,test,Path(__file__),ROOT/'tools/check_storage_image_views.py'];pinned={str(p.relative_to(ROOT)):sha(p) for p in paths}
    text=source.read_text();nt=nvn.read_text()
    types='typedef void *VmaAllocation;typedef struct {unsigned unused;} TexLayout;typedef struct NvnPool NvnPool;\n'
    types+=struct(nt,'typedef struct NvnTexture {','} NvnTexture;')+'\n'
    start=nt.rfind('typedef struct {',0,nt.index('} NvnTextureView;'));types+=nt[start:nt.index('} NvnTextureView;',start)+len('} NvnTextureView;')]+'\n'
    types+=struct(header.read_text(),'typedef struct TexBackend {','} TexBackend;')+'\n'
    (out/'production_types.inc').write_text(types)
    helpers='\n'.join(function(text,n) for n in ['fmt_of','renderer_format_is_depth','renderer_format_has_stencil','vk_view_format'])
    (out/'production_formats.inc').write_text('typedef struct {VkFormat vf;uint8_t expand_rgb,alpha_one;} VkFmt;\n'+helpers+'\n')
    start=text.index('#define VIEW_TAB ');end=text.index('static VkImageViewType view_type_for',start)
    views=text[start:end]+'\n'+function(text,'view_type_for')+'\n'
    start=text.index('static const VkComponentMapping IDENTITY =');views+=text[start:text.index(';',start)+1]+'\n'
    views+='\n'.join(function(text,n) for n in ['vk_color_bytes','vk_block_class','vk_view_compatible','vk_sampled_stencil','vk_view'])+'\n'
    (out/'production_views.inc').write_text(views)
    def run(cmd,log):
        p=subprocess.run(list(map(str,cmd)),cwd=ROOT,capture_output=True,text=True,timeout=90);(out/log).write_text(p.stdout+p.stderr)
        if p.returncode:raise RuntimeError(log+': '+p.stdout+p.stderr)
        return p.stdout.strip()
    reports={};includes=ROOT/'work/las-baseline/build/android/deps/include'
    for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        binary=out/mode
        run([shutil.which('clang'),'-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',*flags,'-I',out,'-I',includes,test,'-o',binary],mode+'-build.log')
        reports[mode]=json.loads(run([binary],mode+'-run.log'));assert reports[mode]['state']=='passed'
    rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/textures.c'))
    overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':row['file'],'external-contents':str(source)},{'type':'file','name':str(Path(row['file']).with_name('vk.h')),'external-contents':str(header)}]}))
    cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(out/'textures-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd,'android-full-tu.log')
    assert pinned=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Sources changed during checks'
    report={'schema':1,'scope':'Actual vk_view, effective aspect classifier and image-view cache with exact NvnTexture/View/TexBackend declarations; controlled Vulkan allocation/upload stand-ins. Full production Android TU compile, no GPU/device qualification.','tests':reports,'android_full_tu':True,'source_sha256':pinned}
    (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
