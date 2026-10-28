#!/usr/bin/env python3
"""Actual feedback selector regression with mocked Vulkan and Android TU compile."""
import hashlib,json,shlex,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/feedback-array-view';SOURCE=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c';TEST=ROOT/'tests/feedback_array_view_test.c'
def run(args,cwd=None):
 p=subprocess.run(list(map(str,args)),cwd=cwd,text=True,capture_output=True,timeout=90)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);source=SOURCE.read_text();a=source.index('static VkImageView feedback_sample_view(');b=source.index('static uint64_t g_cur_fs_hash;',a);fragment=source[a:b];(OUT/'feedback_view.production.h').write_text(fragment)
 assert 'view = feedback_sample_view(k, st_->type, st_->is_uint);' in source
 assert 'if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view' not in source
 assert 'feedback_image[nfeedback] = &existing->copy;' in source and 'feedback_image[nfeedback] = dst;' in source
 cc=shutil.which('clang');assert cc;results={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/name;run([cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',OUT,'-I',ROOT/'ports/shared-android/runtime',TEST,'-o',exe]);results[name]=run([exe]);print(name+': '+results[name],flush=True)
 row=next(e for e in json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text()) if e['file'].endswith('/runtime/nvn/vulkan/exec.c'));overlay=OUT/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/relative),'external-contents':str(ROOT/'ports/shared-android/runtime'/relative)}for relative in ('nvn/vulkan/exec.c','nvn/vulkan/vk.h','nvn/maxwell/shader.h')]}));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(OUT/'exec.android.o');run([*cmd,'-ivfsoverlay',overlay],row['directory'])
 paths=[SOURCE,TEST,Path(__file__)];receipt={'schema':1,'scope':'Actual production feedback selector and real callsite admission; controlled Vulkan view-creation arguments verify same snapshot image, single mip/layer array shape, float depth and integer stencil aspect selection, no source mutation and explicit unsupported shapes. Whole Android production TU compile. Not real Vulkan validation, driver or rift/gameplay proof.','results':results,'android_tu_compiled':True,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},'fragment_sha256':hashlib.sha256(fragment.encode()).hexdigest()};(OUT/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n')
if __name__=='__main__':main()
