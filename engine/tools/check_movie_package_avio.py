#!/usr/bin/env python3
"""Original-package movie AVIO vs existing plaintext reference; no movie extraction or device actions."""
import argparse,hashlib,json,os,re,shlex,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):
 with p.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def fn(text,name):
 m=re.search(r'(?m)^static [^;\n]*\b'+name+r'\(',text);start=m.start();end=text.index('{',m.end())+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--import-receipt',type=Path,required=True);p.add_argument('--inventory',type=Path,required=True);p.add_argument('--movie',default='region_common/movie/rd000_Title.webm');a=p.parse_args()
 out=ROOT/'build/movie-package-avio';out.mkdir(parents=True,exist_ok=True)
 receipt=json.loads(a.import_receipt.read_text());inventory=json.loads(a.inventory.read_text());package=Path(receipt['source']);reference=Path(receipt['destination'])/'romfs'/a.movie
 keys=next(Path(x['path']) for x in inventory.get('key_file_presence_only',inventory.get('key_file_presence_only',[])) if Path(x['path']).is_file())
 source=ROOT/'ports/shared-android/runtime/hle/movie.c';provider=ROOT/'ports/shared-android/runtime/nxpkg.c';test=ROOT/'tests/movie_package_avio_test.c'
 paths=[source,provider,provider.with_suffix('.h'),test,ROOT/'native/runtime/guest_path.c',Path(__file__).resolve()];pinned={str(x.relative_to(ROOT)):sha(x) for x in paths}
 s=source.read_text();start=s.index('typedef struct {\n    AVFormatContext *fmt;');end=s.index('} Extractor;',start)+len('} Extractor;')
 (out/'production_movie_avio.inc').write_text(s[start:end]+'\n'+'\n'.join(fn(s,n) for n in ('img_read','img_seek','ex_close','ex_open'))+'\n')
 def run(cmd,name,timeout=90):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=timeout);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libavformat','libavcodec','libavutil'],text=True))
 before=package.stat();reports={}
 for mode,opt in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/mode;run(['clang','-D_GNU_SOURCE','-std=c11','-Wall','-Wextra','-Werror',*opt,'-I',out,'-I',provider.parent,'-I',ROOT/'native',test,provider,ROOT/'native/runtime/guest_path.c','-pthread',*flags,'-o',exe],mode+'-build.log')
  reports[mode]=json.loads(run([exe,package,keys,a.movie,reference],mode+'-run.log'));assert reports[mode]['state']=='passed'
 after=package.stat();assert (before.st_size,before.st_mtime_ns)==(after.st_size,after.st_mtime_ns)
 assert pinned=={str(x.relative_to(ROOT)):sha(x) for x in paths},'Source changed during verification'
 report={'schema':1,'state':'passed','scope':'Actual production movie custom AVIO callbacks and real NxRomfs provider, real host FFmpeg demux/backward seek/VP9 first-frame decode vs existing extracted reference and raw-image FD. Lookup uses actual package index with a controlled filesystem mount shim. No newly extracted media, no Android playback/device proof.','source_sha256':pinned,'host_ffmpeg':subprocess.check_output(['pkg-config','--modversion','libavformat'],text=True).strip(),'tests':reports,'reference_sha256':sha(reference),'package_size':before.st_size,'package_mtime_ns':before.st_mtime_ns,'private_inputs':{'import_receipt':str(a.import_receipt),'inventory':str(a.inventory)},'device_actions':False}
 (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
