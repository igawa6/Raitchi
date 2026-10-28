#!/usr/bin/env python3
"""Actual per-draw selected vertex metadata, guard-page boundedness and Android TU."""
import hashlib,json,shlex,subprocess
from pathlib import Path
from check_storage_image_views import function
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/material-attributes';SRC=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=90)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);text=SRC.read_text();end=text.index('} VtxLayout;')+len('} VtxLayout;');start=text.rfind('typedef struct {',0,end);types=text[start:end]+'\n';start=text.index('typedef struct { uint64_t addr, size; } Range;');types+=text[start:text.index('\n',start)]+'\n'
 (OUT/'production_attributes.inc').write_text(types+'\n'.join(function(text,n)for n in ['attr_kind','vertex_format','material_attribute_metadata'])+'\n');assert 'material_probe_draw(pr, &pp->k, r, indexed, fii, (unsigned)nruns, &vl);' in text
 results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;run(['clang','-std=c11',*flags,'-I',OUT,'-I',SRC.parent.parent/'maxwell','-I',ROOT/'work/las-baseline/build/android/deps/include',ROOT/'tests/material_attributes_test.c','-o',exe]);results[mode]=run([exe]);print(mode,results[mode],flush=True)
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/exec.c'));overlay=OUT/'overlay.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':row['file'],'external-contents':str(SRC)}]}));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(OUT/'exec-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd)
 paths=[SRC,ROOT/'tests/material_attributes_test.c',Path(__file__)];(OUT/'verification.json').write_text(json.dumps({'scope':'Actual formatter/format tables; actual draw-selected VtxLayout passed in;16attribute bounded reads, protected nextpage, unknown/unprovidedinputs, noGPUqueries/payloadreads. AndroidfullTUcompile, no device actions.','results':results,'android_full_tu':True,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}},indent=2)+'\n');print('AndroidexecTU compiled')
if __name__=='__main__':main()
