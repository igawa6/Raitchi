#!/usr/bin/env python3
"""Actual writable graphics mirror bodies, controlled Vulkan recorder and Android TU compile."""
import argparse,hashlib,json,pathlib,re,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args();out=ROOT/'build/graphics-mirror';out.mkdir(parents=True,exist_ok=True)
 source=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c';test=ROOT/'tests/graphics_mirror_test.c';s=source.read_text();start=s.index('#define MIR_SIZE ');end=s.index('// (guest_direct) a range the GPU holds the current copy of: that copy',start);fragment=s[start:end]
 # This diagnostic helper is interleaved with the mirror implementation in
 # exec.c, but belongs to the stencil probe and has a separate Vulkan/runtime
 # dependency set. Keep the mirror production bodies verbatim while excluding
 # that unrelated helper from this focused extraction.
 probe_start=fragment.index('static void projection_probe_mirror_cb4(void) {')
 probe_end=fragment.index('\nbool vk_gmem_mirror(void)',probe_start)
 fragment=fragment[:probe_start]+fragment[probe_end:]
 (out/'graphics_mirror_fragments.h').write_text(fragment)
 # Actual integration order, including descriptor-cache bypass-independent
 # preparation and readback after all physical draw commands, before submit.
 draw=s[s.index('static void draw_(CmdRec *r, bool indexed)'):s.index('static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {')]
 assert draw.index('mirror_prepare_graphics(pr->vi, pr->fi);')<draw.index('    begin_pass();')<draw.index('VkDescriptorSet vset = stage_set')
 assert draw.index('mirror_finish_graphics(cb);')>draw.index('else rec_CmdDrawIndexed(cb, icount')
 assert draw.index('mirror_finish_graphics(cb);')<draw.index('if (dl_on == 3) renderer_flush();')
 stage=s[s.index('static VkDescriptorSet stage_set('):s.index('// ================================================================== compute') if '// ================================================================== compute' in s else s.index('static void dispatch_compute(')]
 assert stage.index('stage < 2 && mir_graphics_prepared')<stage.index('if (p->reuse && set < 2)')
 assert '&& !mir_graphics_prepared && !uniform_forward_graphics_prepared) renderer_submit();' in s and 'dl_on == 2 && !mir_graphics_prepared' in s
 assert 'storing graphics pipeline failed; refusing to discard shader writes' in draw
 paths=[source,test,pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x) for x in paths}
 def run(args,log,cwd=None):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60,cwd=cwd);(out/log).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(log+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,test,'-pthread','-o',exe],name+'-build.log');reports[name]=json.loads(run([exe],name+'-run.log'))
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(x for x in rows if x['file'].endswith('/runtime/nvn/vulkan/exec.c'));command=shlex.split(row['command']);command[command.index('-o')+1]=str(out/'exec-android.o');stage=ROOT/'work/shared-android/runtime/nvn/vulkan/exec.c';overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(stage),'external-contents':str(source)}]}));run([*command,'-ivfsoverlay',overlay],'android-production-compile.log',row['directory']);android={'passed':True,'object_sha256':sha(out/'exec-android.o')}
 assert pins=={str(x.relative_to(ROOT)):sha(x) for x in paths},'Source changed during checks'
 receipt={'state':'passed','host_tests':reports,'android_production_compile':android,'source_sha256':pins,'fragment_sha256':sha(out/'graphics_mirror_fragments.h'),'scope':'Actual mirror allocator/table/preparation/readback/merge functions with controlled recorder and synthetic storage; cache/integration source ordering assertions. No Vulkan hardware/game/device execution.'};(out/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
