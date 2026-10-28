#!/usr/bin/env python3
"""Actual layout queries and CPU upload rows; no Vulkan/device execution."""
import hashlib,json,shlex,subprocess
from pathlib import Path
from check_storage_image_views import function
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/linear-texture-layout';RT=ROOT/'ports/shared-android/runtime';NVN=RT/'nvn'
def block(s,needle):
 a=s.index(needle);i=s.index('{',a)+1;n=1
 while n:n+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
def run(args,**kw):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=120,**kw)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);o=(NVN/'objects.c').read_text();v=(NVN/'vulkan/textures.c').read_text();e=(NVN/'vulkan/exec.c').read_text()
 queries='\n'.join(function(o,n)for n in('target_is_array','builder_layout','is_linear','builder_storage_size','texture_from_builder'))+'\n'+'\n'.join(block(o,'NVN('+n+')')for n in('nvnTextureBuilderGetStorageSize','nvnTextureBuilderGetStorageAlignment','nvnTextureGetStorageSize','nvnTextureInitialize'))
 (OUT/'queries.inc').write_text(queries)
 upload=function(v[v.rfind('static void upload_texture('):],'upload_texture');a=upload.index('            if (linear) {',upload.index('uint8_t *lin ='));b=upload.index('            if (b->tp > 0',a);rows=upload[a:b];assert 'tegra_deswizzle' in rows and 't->stride' in rows
 (OUT/'rows.inc').write_text('bool linear = nvn_texture_row_linear(t->flags);\n'+rows)
 assert 'if (nvn_texture_row_linear(t->flags)) {' in upload and 'if (t->flags & 0x10) b->dynamic = true;' in upload
 assert upload.count('nvn_texture_row_linear(t->flags) ? -1 : texpatch_marker')==2
 assert 'bool linear = nvn_texture_row_linear(t->flags) || t->target == 10;' in o
 assert 'nvn_texture_row_linear(t->flags)' in function(e,'material_projection_texture_span')
 results={}
 common=['-std=gnu11','-I',OUT,'-I',ROOT/'native','-I',RT,'-I',ROOT/'work/shared-android/runtime/nvn',ROOT/'tests/linear_texture_layout_test.c',NVN/'tegra.c',NVN/'formats.c']
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run(['clang',*common,*flags,'-o',exe]);results[mode]=run([exe]);print(mode,results[mode],flush=True)
 paths=[NVN/f for f in('objects.c','tegra.h','tegra.c','vulkan/textures.c','vulkan/exec.c')];overlay=OUT/'overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/p.relative_to(RT)),'external-contents':str(p)}for p in paths]}));commands=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());compiled=[]
 for suffix in('nvn/objects.c','nvn/tegra.c','nvn/vulkan/textures.c','nvn/vulkan/exec.c'):
  row=next(r for r in commands if r['file'].endswith('/runtime/'+suffix));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(OUT/(Path(suffix).stem+'-android.o'));cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd,cwd=row['directory']);compiled.append(suffix)
 paths += [ROOT/'tests/linear_texture_layout_test.c',Path(__file__)];(OUT/'verification.json').write_text(json.dumps({'results':results,'android_production_TUs':compiled,'scope':'Actual query wrappers, checked row-span and exact actual upload copy branch; linear10/40/50, padding,RGBA16F64x1vsGOBcounterexample,compressedrows,invalidshape/stride/overflow,buffer10tightquery,alignmentunchanged. No driver/GPU execution.','source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}},indent=2)+'\n');print('All4AndroidTUs compiled')
if __name__=='__main__':main()
