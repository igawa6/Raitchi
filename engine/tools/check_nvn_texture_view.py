#!/usr/bin/env python3
"""Actual16B texture-view API/copy bodies and adjacent-client-region regression."""
import hashlib,json,pathlib,re,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];OUT=ROOT/'build/nvn-texture-view';NVN=ROOT/'ports/shared-android/runtime/nvn'
def function(s,signature):
 start=s.index(signature);a=s.index('{',start);depth=1;i=a+1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]+'\n'
def typedef(s,name):
 end=s.index('} '+name+';')+len('} '+name+';');return s[s.rfind('typedef struct',0,end):end]+'\n'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);s=(NVN/'objects.c').read_text();c=(NVN/'cmdbuf.c').read_text();h=(NVN/'nvn.h').read_text()
 start=s.index('// Texture views:');end=s.index('NVN(nvnTextureReadTexels)',start)
 api_names=re.findall(r'NVN\((nvnTextureView\w+)\)',s[start:end]);assert len(api_names)==14 and len(set(api_names))==14
 assert '#define TV(' not in s and 'memcmp(AP(0), AP(1), sizeof(NvnTextureView))' in s
 (OUT/'texture_view_objects.inc').write_text(s[start:end]+function(s,'static void register_texture('))
 command=typedef(c,'Snap')+function(c,'static Snap snap(')+'static CmdRec *record(Cpu *,uint32_t,uint32_t,Snap *);\n'
 command+=typedef(c,'RtRec')+function(c,'NVN(nvnCommandBufferSetRenderTargets)')+typedef(c,'ClearTexRec')+function(c,'static void clear_texture(')+typedef(c,'TexRef')+function(c,'static void tex_ref(')
 e=(NVN/'vulkan/exec.c').read_text()
 for name,pattern,producer,fields in [
  ('ReplayRtRec',r'const struct \{ uint64_t tex\[8\]; NvnTextureView views\[8\]; uint8_t has_view\[8\]; uint64_t depth;\s*NvnTextureView dview; uint8_t has_dview; \} \*rt = x;', 'RtRec',('tex','views','has_view','depth','dview','has_dview')),
  ('ReplayClearRec',r'const struct \{ uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region\[6\]; float color\[4\]; \} \*ct = x;', 'ClearTexRec',('tex','view','has_view','region','color'))]:
  matched=re.search(pattern,e);assert matched,'Replay record shape changed: '+name
  body=matched.group(0);body=body[body.index('{'):body.rindex('}')+1]
  command+='typedef struct '+body[1:-1].join(['{','}'])+' '+name+';\n'
  command+='_Static_assert(sizeof('+producer+')==sizeof('+name+'),"view record extent");\n'
  for field in fields:command+='_Static_assert(offsetof('+producer+','+field+')==offsetof('+name+','+field+'),"view record field");\n'
 command+=typedef(e,'TexRefRec')+'_Static_assert(sizeof(TexRef)==sizeof(TexRefRec),"copy record extent");\n'
 objects=(NVN/'objects.c').read_text()
 command+=function(objects,'bool nvn_pool_resolve_span(')
 for field in ('tex','view','has_view','region'):command+='_Static_assert(offsetof(TexRef,'+field+')==offsetof(TexRefRec,'+field+'),"copy record field");\n'
 (OUT/'texture_view_commands.inc').write_text(command)
 files=[NVN/'nvn.h',NVN/'objects.c',NVN/'cmdbuf.c',NVN/'vulkan/exec.c',NVN/'tegra.h',NVN/'tegra.c',NVN/'formats.c',NVN/'pool_mapping.h',NVN/'pool_mapping.c',ROOT/'tests/nvn_texture_view_test.c',pathlib.Path(__file__)];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
 results={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=OUT/name;run([shutil.which('clang'),'-std=gnu11','-Wall','-Wextra','-Werror',*flags,'-I',OUT,'-I',ROOT/'native','-I',NVN.parent,'-I',ROOT/'work/shared-android/runtime/nvn',ROOT/'tests/nvn_texture_view_test.c',NVN/'tegra.c',NVN/'formats.c',NVN/'pool_mapping.c','-pthread','-o',binary]);results[name]=run([binary])
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'Source changed during checks'
 report={'state':'passed','results':results,'actual_api_names':api_names,'source_sha256':pins,'scope':'Actual14 intercepted TextureView APIs and actual descriptor/copy/render-target/clear view capture bodies; real GOB4x1 RGBA8 WriteTexels with16Bview directly preceding initialized24Bregion. Guarded destinations, exact-size ASan allocations, transactional range rejection; no original-driver wire-layout or device rendering claim.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
