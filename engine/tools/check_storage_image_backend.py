#!/usr/bin/env python3
"""Test actual storage-image binding/ownership functions without game assets or Vulkan hardware."""
import argparse,hashlib,json,pathlib,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args();out=ROOT/'build/storage-image-backend';out.mkdir(parents=True,exist_ok=True)
 source=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c';test=ROOT/'tests/storage_image_backend_test.c';s=source.read_text();fragment=s[s.index('static NvnTexture *storage_written[64];'):s.index('// Writes the descriptor set of one stage.')];(out/'storage_image_fragments.h').write_text(fragment)
 stage=s[s.index('static VkDescriptorSet stage_set('):s.index('static void dispatch_compute(')]
 assert stage.index('b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE')<stage.index('if (p->reuse && set < 2)')
 assert 'ii[n] = storage_image_info(stage, si, b->binding - 32);' in stage and 'w[n].pImageInfo = &ii[n];' in stage
 dispatch=s[s.index('static void dispatch_compute('):s.index('// ================================================================== draws')]
 assert dispatch.index('vk_end_pass();')<dispatch.index('storage_nwritten = 0;')<dispatch.index('VkDescriptorSet ds = stage_set(')<dispatch.index('rec_CmdDispatch(')<dispatch.index('storage_images_finish(cb);')<dispatch.index('stat_dispatches++;')
 pins={str(x.relative_to(ROOT)):sha(x) for x in (source,test,pathlib.Path(__file__).resolve())}
 def run(args,name,cwd=None):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60,cwd=cwd);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,'-I',ROOT/'ports/shared-android/runtime',test,'-o',exe],name+'-build.log');reports[name]=json.loads(run([exe],name+'-run.log'))
 # The real regression must reject the previous nonzero-handle assumption.
 mutant=fragment.replace('if (!st.tpool || !st.tpool->entries ||','if (!handle || !st.tpool || !st.tpool->entries ||',1)
 assert mutant!=fragment
 mutant_dir=out/'zero-mutant';mutant_dir.mkdir(exist_ok=True);(mutant_dir/'storage_image_fragments.h').write_text(mutant)
 exe=mutant_dir/'test';run([shutil.which('clang'),'-std=c11','-O2','-I',mutant_dir,'-I',ROOT/'ports/shared-android/runtime',test,'-o',exe],'zero-mutant-build.log')
 r=subprocess.run([exe],capture_output=True,text=True,timeout=10);(out/'zero-mutant-run.log').write_text(r.stdout+r.stderr)
 assert r.returncode!=0 and 'missing or out-of-range texture handle' in r.stderr,'Old zero-handle rejection escaped regression'
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(x for x in rows if x['file'].endswith('/runtime/nvn/vulkan/exec.c'));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(out/'exec-android.o');overlay=out/'overlay.json';files=['nvn/vulkan/exec.c','nvn/vulkan/vk.h','nvn/maxwell/shader.h'];overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/f),'external-contents':str(ROOT/'ports/shared-android/runtime'/f)} for f in files]}));run([*cmd,'-ivfsoverlay',overlay],'android-production-compile.log',row['directory']);android={'passed':True,'object_sha256':sha(out/'exec-android.o')}
 assert pins=={k:sha(ROOT/k) for k in pins},'Owned source changed during checks'
 report={'state':'passed','host_tests':reports,'old_zero_handle_mutant_rejected':True,'android_production_compile':android,'source_sha256':pins,'fragment_sha256':sha(out/'storage_image_fragments.h'),'scope':'Actual exec binding/ownership helpers with mocked texture allocator/commands; source integration ordering. Does not establish real format, GPU storage-write semantics, or game visual correctness.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
