#!/usr/bin/env python3
"""Actual bounded CPU texture export + real Tegra layout and kernel readable copy."""
import hashlib,json,re,shlex,subprocess
from pathlib import Path
from check_storage_image_views import function,struct
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/projection-texels';RT=ROOT/'ports/shared-android/runtime'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=120)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);src=RT/'nvn/vulkan/exec.c';s=src.read_text();n=(RT/'nvn/nvn.h').read_text();v=(RT/'nvn/vulkan/vk.h').read_text();types='typedef struct NvnPoolMappings NvnPoolMappings;\n'+struct(n,'typedef struct NvnPool {','} NvnPool;')+'\n'+struct(n,'typedef struct NvnTexture {','} NvnTexture;')+'\n'
 for endmark in ['} NvnTextureView;','} NvnTexEntry;']:
  end=n.index(endmark)+len(endmark);start=n.rfind('typedef struct {',0,end);types+=n[start:end]+'\n'
 types+=struct(n,'typedef struct NvnDescPool {','} NvnDescPool;')+'\n'+struct(v,'typedef struct TexBackend {','} TexBackend;')+'\n';(OUT/'types.inc').write_text(types)
 decl=s[s.index('#define MATERIAL_PROJECTION_MAX_BYTES'):s.index('static bool material_projection_range(')];(OUT/'export.inc').write_text(decl+'\n'+'\n'.join(function(s,name)for name in ['material_projection_range','material_projection_payload','material_projection_texture_span','material_projection_texels']))
 k=(RT/'kernel.h').read_text();(OUT/'kernel.inc').write_text(re.search(r'(?m)^#define KERNEL_READABLE_COPY_MAX[^\n]+',k)[0]+'\n'+function((RT/'kernel.c').read_text(),'kernel_copy_readable'))
 assert s.index('material_projection_texels(&cap, p);')>s.index('free(material_projection_payload(&cap, name, st.vbuf[s].addr, size));')
 results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run(['clang','-std=c11',*flags,'-I',OUT,'-I',RT/'nvn','-I',RT/'nvn/maxwell','-I',ROOT/'native','-I',ROOT/'work/las-baseline/build/android/deps/include',ROOT/'tests/projection_texels_test.c',RT/'nvn/tegra.c',RT/'nvn/formats.c','-pthread','-o',exe]);results[mode]=run([exe]);print(mode,results[mode],flush=True)
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/exec.c'));overlay=OUT/'overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':row['file'],'external-contents':str(src)},{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/tegra.h'),'external-contents':str(ROOT/'ports/shared-android/runtime/nvn/tegra.h')}]}));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(OUT/'exec-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd)
 paths=[src,RT/'nvn/tegra.c',RT/'kernel.c',ROOT/'tests/projection_texels_test.c',Path(__file__)];(OUT/'verification.json').write_text(json.dumps({'results':results,'android_full_tu':True,'scope':'Actual exact texture/pool/backend types, real Tegra layout+kernel copy, controlled descriptor resolver/metadata formatter; smallCPU originals only,64KiB/texture,8entries,geometry-first2MiBcap,pool/layout/overflow/ownedrejects,guardpage proof failure. No Vulkan calls/device/GPU pixels.','source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}},indent=2)+'\n');print('AndroidexecTU compiled')
if __name__=='__main__':main()
