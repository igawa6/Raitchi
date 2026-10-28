#!/usr/bin/env python3
"""Actual requested clear diagnostic, ordinary/explicit rectangle routing and Android TU."""
import hashlib,json,shlex,subprocess
from pathlib import Path
from check_storage_image_views import function,struct
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/clear-scope-probe';SRC=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=120)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);text=SRC.read_text();nt=(SRC.parent.parent/'nvn.h').read_text()
 types='typedef struct {void*fmt;}TexLayout;typedef struct NvnPool NvnPool;\n'+struct(nt,'typedef struct NvnTexture {','} NvnTexture;')+'\n';end=nt.index('} NvnTextureView;')+len('} NvnTextureView;');start=nt.rfind('typedef struct {',0,end);types+=nt[start:end];(OUT/'types.inc').write_text(types)
 (OUT/'scope.inc').write_text('static int clear_scope_test_enabled=-1;\n'+'\n'.join(function(text,n)for n in ['clear_scope_full_test','clear_scope_rect','clear_scope_covers','clear_scope_attachment_covers','pass_size','scissor_rect']))
 start=text.index('static uint64_t material_projection_fs =');end=text.index('typedef struct {\n    FILE *meta, *data;');selector='static uint64_t material_projection_fs; static bool material_projection_full_stage_hash;\n'+function(text,'material_projection_selector')
 start=text.index('static int material_probe_enabled =');end=text.rfind('static void material_probe_view(FILE *f,');gate=text[start:end]
 (OUT/'gate.inc').write_text(selector+'\n'+gate+'\n'+function(text[text.rfind('static void material_probe_view('):],'material_probe_view')+'\n'+function(text[text.rfind('static void material_probe_clear('):],'material_probe_clear'))
 (OUT/'clear.inc').write_text(function(text,'clear'))
 assert 'clear_scope_covers(d->width, d->height, st.scissor[0])'in text
 assert 'clear_scope_attachment_covers(c->width' in text
 assert 'ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0, true)'in text
 results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run(['clang','-std=c11',*flags,'-I',OUT,'-I',ROOT/'work/las-baseline/build/android/deps/include',ROOT/'tests/clear_scope_probe_test.c','-lm','-o',exe]);results[mode]=run([exe]);print(mode,results[mode],flush=True)
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/exec.c'));overlay=OUT/'overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':row['file'],'external-contents':str(SRC)}]}));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(OUT/'exec-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd)
 paths=[SRC,ROOT/'tests/clear_scope_probe_test.c',Path(__file__)];(OUT/'verification.json').write_text(json.dumps({'scope':'Actual clear/gate/formatter and scissor/policy helpers, controlled Vulkan recorder only; request gates, leading clears,256cap/saturatingoverflow,frame reset,normal/full/explicit regions,masks/values/aliascoverage,integer edgecases. No GPU/device execution.','results':results,'android_full_tu':True,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}},indent=2)+'\n');print('AndroidexecTU compiled')
if __name__=='__main__':main()
