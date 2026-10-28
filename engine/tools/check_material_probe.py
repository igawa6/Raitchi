#!/usr/bin/env python3
"""Bounded CPU-only draw metadata probe verification; no game/GPU/device access."""
import argparse,hashlib,json,pathlib,re,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args();out=ROOT/'build/material-probe';out.mkdir(parents=True,exist_ok=True);source=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c';test=ROOT/'tests/material_probe_test.c';s=source.read_text();start=s.index('// LARECOMP_MATERIAL_PROBE=1 + material-probe.request:');end=s.index('// ================================================================== texture copies',start);fragment=s[start:end];(out/'material_probe_fragments.h').write_text(fragment)
 # The diagnostic can inspect metadata/counters and write its own file only.
 assert set(re.findall(r'\b(vk_\w+)\s*\(',fragment))=={'vk_frame_counter'}
 assert not re.search(r'\b(?:rec_|renderer_|vma|SDL_|shader_stage_hash|debug_draw)\w*\s*\(',fragment)
 draw=s[s.index('static void draw_(CmdRec *r, bool indexed) {'):s.index('static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {')]
 assert draw.index('material_probe_poll();')<draw.index('get_program(')
 # A memoized pipeline may never initialize local k. Log the selected pipeline's
 # immutable key, which is also the state actually used to build that pipeline.
 assert draw.index('material_probe_draw(pr, &pp->k, r, indexed, fii, (unsigned)nruns, &vl);')<draw.index('if (!use_index) rec_CmdDraw(')
 assert 'material_probe_draw(pr, &k,' not in draw
 assert 'kernel_copy_readable(bytes, address, size)' in fragment
 assert 'sampled_texture_handle(1, image, &handle)' in fragment
 assert 'shader_code_hash(bytes, (size_t)(instructions + constants))' in fragment
 paths=[source,test,pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x) for x in paths}
 def run(args,name,cwd=None):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60,cwd=cwd);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,'-I',ROOT/'ports/shared-android/runtime','-I',ROOT/'native',test,'-o',exe],name+'-build.log');reports[name]=json.loads(run([exe],name+'-run.log'))
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(x for x in rows if x['file'].endswith('/runtime/nvn/vulkan/exec.c'));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(out/'exec-android.o');overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/vulkan/exec.c'),'external-contents':str(source)}]}));run([*cmd,'-ivfsoverlay',overlay],'android-production-compile.log',row['directory']);android={'passed':True,'object_sha256':sha(out/'exec-android.o')}
 assert pins=={k:sha(ROOT/k) for k in pins},'Source changed during tests'
 report={'state':'passed','host_tests':reports,'android_production_compile':android,'source_sha256':pins,'fragment_sha256':sha(out/'material_probe_fragments.h'),'scope':'Actual finite material logger and bounded ProjectionMask CPU-source sidecar; synthetic draw buffers, self-memory reads, active-window/guard/budget/error checks, one capture per requested present. Source bytes are explicitly not GPU-cache readback. No game data or device/GPU behavior verified.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
