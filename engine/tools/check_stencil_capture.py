#!/usr/bin/env python3
"""Verify actual bounded stencil readback/writer, files and Android production TU."""
import hashlib,json,shlex,shutil,subprocess
from pathlib import Path
from check_storage_image_views import function,struct
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 out=ROOT/'build/stencil-capture';out.mkdir(parents=True,exist_ok=True);runtime=ROOT/'ports/shared-android/runtime';source=runtime/'nvn/vulkan/exec.c';header=runtime/'nvn/vulkan/vk.h';nvn=runtime/'nvn/nvn.h';test=ROOT/'tests/stencil_capture_test.c';paths=[source,header,nvn,test,Path(__file__)];forward=runtime/'nvn/vulkan/uniform_forward.h';paths+=([forward] if forward.exists() else []);pins={str(p.relative_to(ROOT)):sha(p)for p in paths};text=source.read_text()
 types='typedef void *VmaAllocation;typedef struct {unsigned unused;} TexLayout;typedef struct NvnPool NvnPool;\n'+struct(nvn.read_text(),'typedef struct NvnTexture {','} NvnTexture;')+'\n'+struct(header.read_text(),'typedef struct TexBackend {','} TexBackend;')+'\n';(out/'production_types.inc').write_text(types)
 mirror='#define MIR_SLOTS 128\n#define MIR_SIZE (64ull<<20)\n#define MIR_RB 4\n'+struct(text,'typedef struct {\n    uint64_t lo, hi, off, gpu_seq, last_use;','} MirSlot;')+'\n';(out/'production_mirror_types.inc').write_text(mirror)
 start=text.index('#define STENCIL_PROBE_PLANES ');end=text.index('// (debug) LARECOMP_COPY_TRACE=',start);fragment=text[start:end];assert 'renderer_flush(' not in fragment and 'vk_wait' not in fragment and 'renderer_submit(' not in fragment;(out/'production_stencil_capture.inc').write_text(fragment+'\n'+function(text,'stencil_probe_consumers')+'\n'+function(text,'projection_sampled_image')+'\n'+function(text,'projection_probe_before_draw')+'\n'+function(text,'projection_probe_after_draw')+'\n'+function(text[text.index('static void projection_probe_mirror_cb4(void) {'):],'projection_probe_mirror_cb4')+'\n')
 assert text.index('projection_probe_before_draw(pr);') < text.index('mirror_prepare_graphics(pr->vi, pr->fi);')
 assert text.index('projection_probe_after_draw();',text.index('mirror_finish_graphics(cb);')) > text.index('mirror_finish_graphics(cb);')
 assert text.index('stencil_probe_consumers(pr);') < text.index('mirror_prepare_graphics(pr->vi, pr->fi);')
 assert not any(x in function(text,'stencil_probe_consumers') for x in ('renderer_flush(', 'vk_wait', 'renderer_submit(', 'renderer_present_count('))
 assert 'renderer_present_count(' not in fragment
 assert 'stencil probe frame %llu ended: uint_candidates' in text
 assert 'debug_stencil_pair(t, src, &existing->copy, true);' in text and 'debug_stencil_pair(t, src, dst, false);' in text
 snap=function(text,'snapshot_image');assert 'VK_IMAGE_USAGE_TRANSFER_SRC_BIT' in snap and 'd->usage = ci.usage;' in snap
 def run(cmd,log,cwd=ROOT):
  p=subprocess.run(list(map(str,cmd)),cwd=cwd,capture_output=True,text=True,timeout=90);(out/log).write_text(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(log+': '+p.stdout+p.stderr)
  return p.stdout.strip()
 reports={};includes=ROOT/'work/las-baseline/build/android/deps/include'
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  folder=out/(mode+'-files');shutil.rmtree(folder,ignore_errors=True);(folder/'build').mkdir(parents=True);binary=out/mode
  run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',*flags,'-pthread','-I',out,'-I',includes,'-I',runtime,test,'-o',binary],mode+'-build.log');reports[mode]=json.loads(run([binary,folder],mode+'-run.log'))
  raw=(folder/'known.pgm').read_bytes();assert raw==b'P5 64 4 255\n'+bytes(range(256))
  j=json.loads((folder/'known.pgm.json').read_text());assert j['schema']==2 and j['seq']==5 and j['pgm_written'];assert j['histogram']==[1]*256 and j['bit_counts']==[128]*8 and j['image']=='111' and j['source_image']=='111' and j['snapshot_image']=='222' and j['texture_guest']=='333' and j['frame']==77 and not j['reused']
  assert not (folder/'fail-invalidate.pgm').exists() and not (folder/'fail-alloc.pgm').exists()
  files=list((folder/'build').glob('*.pgm'));assert len(files)==22
  for role,fmt,image in [('input-t3',37,'aaa'),('rt0-after',37,'bbb'),('rt1-after',122,'ccc')]:
   raw=(folder/f'build/projection_84_{role}.bin').read_bytes();assert raw==bytes(range(256))*4
   meta=json.loads((folder/f'build/projection_84_{role}.bin.json').read_text());assert meta['format']==fmt and meta['image']==image and meta['encoding']=='raw-color32' and meta['seq']==13 and meta['frame']==84 and meta['pgm_written']
  for role,fmt,image in [('depth-t0',130,'111'),('depth-t2',129,'ddd'),('depth-rt',126,'eee')]:
   raw=(folder/f'build/projection_84_{role}.bin').read_bytes();assert raw==bytes(range(256))*4
   meta=json.loads((folder/f'build/projection_84_{role}.bin.json').read_text());assert meta['format']==fmt and meta['image']==image and meta['encoding']=='raw-depth32' and meta['seq']==13
  uniform=(folder/'build/projection_84_gpu-cb4.bin').read_bytes();assert uniform==bytes(range(256))*3
  j=json.loads((folder/'build/projection_84_gpu-cb4.bin.json').read_text());assert j['encoding']=='raw-uniform' and j['bytes']==768 and j['owner_lo']=='1000' and j['owner_hi']=='2000' and j['owner_off']==48<<20 and j['owner_gpu_seq']==8 and j['owner_last_use']==12 and j['observer_last_use']==13 and j['source_buffer_offset']==(48<<20)+256 and j['source_cbuf_addr']=='1100' and j['seq']==13
  assert len(list((folder/'build').glob('projection_*.bin')))==7
  reused=json.loads((folder/'build/stencil_78_00_snap.pgm.json').read_text());assert reused['reused'] and reused['role']=='snapshot' and reused['image']=='222'
  sampled=json.loads((folder/'build/stencil_82_sampled_00.pgm.json').read_text());assert sampled['role']=='sampled' and sampled['image']=='111' and sampled['source_image']=='111' and sampled['snapshot_image']=='0' and sampled['sampled_stage']==1 and sampled['handle_offset']==32
  sampled=json.loads((folder/'build/stencil_82_sampled_02.pgm.json').read_text());assert sampled['role']=='sampled' and sampled['image']=='222' and sampled['source_image']=='111' and sampled['snapshot_image']=='222'
  sampled=json.loads((folder/'build/stencil_82_sampled_03.pgm.json').read_text());assert sampled['image']=='222' and sampled['source_image']=='222' and sampled['snapshot_image']=='0'
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/nvn/vulkan/exec.c'))
 overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/r),'external-contents':str(runtime/r)}for r in (['nvn/vulkan/exec.c','nvn/vulkan/vk.h','nvn/maxwell/shader.h']+(['nvn/vulkan/uniform_forward.h'] if forward.exists() else []))]}));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(out/'exec-android.o');cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd,'android-full-tu.log',row['directory'])
 assert pins=={str(p.relative_to(ROOT)):sha(p)for p in paths},'Sources changed during checks'
 report={'schema':1,'scope':'Actual standalone request/one-present admission, asynchronous seq-gated Vulkan readback retirement and pair capture and independent producer/renderer frame-domain regression, bounded request summary, ProjectionMask-specific pre-input/post-draw color/depth lifetime and strict37/122 color,D32/D24 depth admission, CB4 mirror-owner buffer readback with pending lifetime protection and unowned rejection, actual nonattachment UINT consumer binding selection/deduplication (including aliases and feedback replacement) with exact texture declarations; controlled GPU/VMA operations, real PGM/JSON byte and histogram validation. Full Android exec.c TU compile. No device or real GPU/content proof.','tests':reports,'android_full_tu':True,'source_sha256':pins};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
