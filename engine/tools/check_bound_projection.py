#!/usr/bin/env python3
"""Verify actual bound-staging capture and actual guest-copy cache without Vulkan execution."""
import argparse,hashlib,json,pathlib,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');args=p.parse_args();out=ROOT/'build/bound-projection';out.mkdir(parents=True,exist_ok=True)
 runtime=ROOT/'ports/shared-android/runtime';source=runtime/'nvn/vulkan/exec.c';test=ROOT/'tests/bound_projection_test.c';paths=[source,test,pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x)for x in paths};text=source.read_text()
 start=text.index('#define GCOPY_TAB ');end=text.index('// ================================================================== worker threads',start);(out/'bound_projection.cache.h').write_text(text[start:end])
 start=text.index('static bool material_probe_bound_enabled(void);');end=text.index('static bool mirror_uniform_info(',start);(out/'bound_projection.production.h').write_text(text[start:end])
 cbuf=text[end:text.index('// Global memory (shader LDG/STG',end)];assert 'material_bound_projection(stage, k, span, zero_size, 0, true);' in cbuf;assert 'material_bound_projection(stage, k, span, size, r.addr, false);\n    return ubo_span(span, size);' in cbuf
 assert 'return material_probe_file && material_probe_count < MATERIAL_PROBE_MAX_DRAWS' in text
 def run(cmd,name,cwd=None):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,cwd=cwd,timeout=120);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 host={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,'-I',out,test,'-o',exe],name+'-build.log');host[name]=json.loads(run([exe],name+'-run.log'))
 android={}
 if args.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/runtime/nvn/vulkan/exec.c'));overlay=out/'overlay.json';owned=[source,runtime/'nvn/vulkan/mirror_normalize.h',runtime/'nvn/vulkan/uniform_forward.h',runtime/'nvn/vulkan/vk.h',runtime/'nvn/maxwell/shader.h'];overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/f.relative_to(runtime)),'external-contents':str(f)}for f in owned]}));cmd=shlex.split(row['command']);obj=out/'exec-android.o';cmd[cmd.index('-o')+1]=str(obj);run([*cmd,'-ivfsoverlay',overlay],'android-production-compile.log',row['directory']);android={'passed':True,'object_sha256':sha(obj)}
 assert pins=={str(x.relative_to(ROOT)):sha(x)for x in paths},'Source changed during checks'
 receipt={'state':'passed','host':host,'android':android,'source_sha256':pins,'scope':'Actual production span-capture and guest-copy cache bodies. Cached bytes differ from current guest source and are captured unchanged. Existing request/hash/stage/frame gating, once-per-frame, explicit zero-buffer bytes, range limit,8192Bprefix, unmapped/overflow failure. CPU staging capture only, no GPU flush/readback or descriptor/cache-policy change.'};(out/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
