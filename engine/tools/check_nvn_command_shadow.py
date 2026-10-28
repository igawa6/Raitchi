#!/usr/bin/env python3
"""Actual shared NVN recorder/shadow tests with bounded synthetic guest chunks."""
import argparse,hashlib,json,pathlib,shutil,subprocess,shlex,os
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');a=p.parse_args()
 out=ROOT/'build/nvn-command-shadow';(out/'nvn').mkdir(parents=True,exist_ok=True)
 runtime=ROOT/'ports/shared-android/runtime';stage=ROOT/'work/shared-android/runtime';source=runtime/'nvn/cmdbuf.c';header=runtime/'nvn/nvn.h';test=ROOT/'tests/nvn_command_shadow_test.c'
 shutil.copyfile(source,out/'nvn/production_cmdbuf.c');shutil.copyfile(header,out/'nvn/nvn.h');shutil.copyfile(runtime/'nvn/tegra.h',out/'nvn/tegra.h');shutil.copyfile(runtime/'nvn/pool_mapping.h',out/'nvn/pool_mapping.h');shutil.copyfile(runtime/'nvn/command_pool_stats.h',out/'nvn/command_pool_stats.h')
 # cmdbuf.c now includes the real lifetime integration interface. This
 # recorder fixture deliberately exercises only the legacy mode: it compiles
 # production cmdbuf.c with the current headers and fail-fast OFF stand-ins.
 for name in ['command_recording.h','command_lifetime.h','command_body_arena.h']:
  shutil.copyfile(runtime/'nvn'/name,out/'nvn'/name)
 shutil.copyfile(runtime/'nvn/command_recording.c',out/'nvn/production_command_recording.c')
 shutil.copyfile(runtime/'nvn/rqueue.h',out/'nvn/rqueue.h')
 (out/'kernel.h').write_text('#pragma once\n#include <stdbool.h>\n#include <stdint.h>\nbool kernel_copy_readable(void *,uint64_t,uint64_t);\n')
 (out/'platform.h').write_text('#pragma once\n#include <stdint.h>\nuint64_t plat_now_ns(void);\n')
 replay_source=runtime/'nvn/vulkan/exec.c';replay=replay_source.read_text();start=min(replay.index('        case '+name+':')for name in ['CMD_BIND_UNIFORM_BUFFERS','CMD_BIND_UNIFORM_BUFFERS_DEFERRED']);end=replay.index('        case CMD_SET_TEXTURE_POOL:',start);fragment=replay[start:end]
 for name in ['CMD_BIND_TEXTURES_DEFERRED','CMD_BIND_UNIFORM_BUFFERS_DEFERRED','CMD_BIND_STORAGE_BUFFERS_DEFERRED']:assert name in fragment
 (out/'nvn/bind_replay_production.inc').write_text(fragment)
 objects=runtime/'nvn/objects.c';body=objects.read_text();start=body.index('NVN(nvnQueueSubmitCommands)');end=body.index('__thread uint64_t g_submit_ns',start);(out/'nvn/queue_submit_production.inc').write_text(body[start:end])
 (out/'hle.h').write_text('''#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "runtime/aot_cpu.h"
#define HLE_FUNC(symbol,fn) static void __attribute__((unused)) fn(Cpu *c)
#define RT_ON_RESET(fn) static void __attribute__((unused)) fn(void)
#define GUEST_LO UINT64_C(0x10000)
#define GUEST(x) (x)
void shadow_log(const char *format,...);
#define LOG(...) shadow_log(__VA_ARGS__)
#define FATAL(...) fatal(__VA_ARGS__)
void guest_call(Cpu *,uint64_t);
''')
 ring_test=ROOT/'tests/command_recording_ring_test.c'
 ring_sources=[runtime/'nvn/command_lifetime.c',runtime/'nvn/command_body_arena.c']
 paths=[source,header,runtime/'nvn/tegra.h',runtime/'nvn/pool_mapping.h',runtime/'nvn/command_pool_stats.c',runtime/'nvn/command_pool_stats.h',*[runtime/'nvn'/name for name in ['command_recording.h','command_recording.c','rqueue.h','command_lifetime.h','command_body_arena.h']],*ring_sources,replay_source,objects,test,ring_test,ROOT/'native/runtime/aot_cpu.h',pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x) for x in paths}
 def run(args,log,env=None):
  x=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60,env=env);(out/log).write_text(x.stdout+x.stderr)
  if x.returncode:raise RuntimeError(log+': '+x.stdout+x.stderr)
  return x.stdout
 common=['-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I',ROOT/'native','-I',out/'nvn',runtime/'nvn/command_pool_stats.c'];reports={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/mode;run([shutil.which('clang'),*common,*flags,test,'-pthread','-o',binary],mode+'-build.log')
  env=dict(os.environ);env.pop('LARECOMP_TRACE_COMMAND_MEMORY',None)
  reports[mode]=json.loads(run([binary],mode+'-run.log',env))
  env['LARECOMP_TRACE_COMMAND_MEMORY']='1';reports[mode+'-trace']=json.loads(run([binary],mode+'-trace-run.log',env))
  ring_binary=out/(mode+'-ring');run([shutil.which('clang'),*common,*flags,'-DNVN_COMMAND_LIFETIME_HOST_TEST=1',ring_test,*ring_sources,'-pthread','-o',ring_binary],mode+'-ring-build.log')
  env['LARECOMP_COMMAND_LIFETIME']='1';env.pop('LARECOMP_TRACE_COMMAND_MEMORY',None)
  reports[mode+'-ring-on']=json.loads(run([ring_binary],mode+'-ring-run.log',env))
 target=ROOT.parent/'android-sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang';binary=out/'android-shadow-test'
 run([target,*common,'-O2','-ffixed-x18',test,'-pthread','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',binary],'android-test-link.log')
 run([target,*common,'-O2','-ffixed-x18','-DNVN_COMMAND_LIFETIME_HOST_TEST=1',ring_test,*ring_sources,'-pthread','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',out/'android-ring-test'],'android-ring-link.log')
 actual={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(x for x in rows if x['file'].endswith('/runtime/nvn/cmdbuf.c'));command=shlex.split(row['command']);command[command.index('-o')+1]=str(out/'cmdbuf-android.o')
  # Quote-relative nvn.h must resolve as the staged path everywhere, including
  # its existing pragma-once identity in other production headers.
  overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(stage/'nvn/cmdbuf.c'),'external-contents':str(source)},{'type':'file','name':str(stage/'nvn/nvn.h'),'external-contents':str(header)}]}))
  command+=['-ivfsoverlay',str(overlay)];run(command,'android-production-compile.log');actual={'passed':True,'object_sha256':sha(out/'cmdbuf-android.o')}
 assert pins=={str(x.relative_to(ROOT)):sha(x) for x in paths},'Sources changed during check'
 record={'state':'passed','host_tests':reports,'android_cross_link_passed':True,'android_production_compile':actual,'source_sha256':pins,'scope':'Production cmdbuf.c and actual NVN layouts in legacy command-lifetime-OFF mode, plus a focused lifetime-ON ring fixture compiling the actual command_recording.c bridge, allocator and lifetime core with kernel/platform/queue stand-ins. Both routes cover completed-frame tail/prefix recycling, nested exhaustion, overlap rejection, lower-span retention, payload preservation and Begin resets; OFF also injects prefix allocation failure and preserves old host roots, while ON verifies disjoint published spans, pinned old bodies and empty rebind to a new logical pool generation. GPU execution and asynchronous queue/device behavior are not established. Independent word-aligned HLE budget (not exact GPU packet sizes), host-only Begin/one-word End, retained multi-spill empty/payload/fenced lists, callback after spills/reset/args preservation, explicit nominal over-reservation bounded by4/8/472Bpool tails, allocation-failure rollback,4KiB initial roots with unchanged32KiB incremental continuations and actual allocated-capacity checks, natural root-boundary spill and repeated-reuse plateau, actual64KiB inline growth,96unique16MiB-tail reservations retaining under400KiB versus an8GiB+ eager ceiling, precise failure accounting, stable4MiB slabs with4096 packed blocks using4 backing allocations,16B alignment, distinct adjacent-block bounds, fresh/existing allocation-failure rollback, >4MiB actual-record reuse and slab cleanup once, sorted allocation interval index with failure rollback and cleanup, 20,000 unrelated allocations with at most15 binary-search comparisons per block and sequential bounds reuse, exact-host-root continuation leases with fixed-root retained-byte plateau, mixed-capacity/once-per-cycle selection, old-size-generation and called/appended-child preservation, permanent pin before Begin writes, callback ownership, root-table rehash/failure rollback, deferred record/replay/QueueSubmit array lifetime and legal rerecord after consumption, guest/redzone canaries and cleanup. Only continuations are reused; unique root backing and pinned blocks remain retained. All generations remain retained until GPU-idle cleanup; no reclamation or device/GPU qualification'};(out/'verification.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
if __name__=='__main__':main()
