#!/usr/bin/env python3
"""Actual registered offline network/account/applet handlers with synthetic IPC."""
import hashlib,json,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
OUT=ROOT/'build/offline-services'
FILES=[ROOT/'ports/shared-android/runtime/services'/n for n in ('network_offline.c','acc.c','am.c')]
HEADER=ROOT/'ports/shared-android/runtime/online_guard.h'
IPC=ROOT/'ports/shared-android/runtime/ipc.c'
IPCH=ROOT/'ports/shared-android/runtime/ipc.h'
KERNEL=ROOT/'ports/shared-android/runtime/kernel.c'
KERNELH=ROOT/'ports/shared-android/runtime/kernel.h'
TEST=ROOT/'tests/offline_services_test.c'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 OUT.mkdir(parents=True,exist_ok=True);(OUT/'services').mkdir(exist_ok=True)
 pins={str(p.relative_to(ROOT)):sha(p)for p in [*FILES,HEADER,IPC,IPCH,KERNEL,KERNELH,TEST,pathlib.Path(__file__)]}
 for p in FILES:shutil.copyfile(p,OUT/'services'/p.name)
 shutil.copyfile(HEADER,OUT/'online_guard.h');shutil.copyfile(IPCH,OUT/'ipc.h')
 (OUT/'kernel.h').write_text('''#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
typedef struct { uint64_t tpidrro; } Cpu;
enum { KOBJ_THREAD=1, KOBJ_EVENT=2, KOBJ_SESSION=3 };
typedef struct { int type, refs; bool signaled; } KObject;
typedef KObject KEvent;
KObject *handle_get(uint32_t);
uint32_t kernel_new_handle(KObject *);
void kernel_close_service_handle(uint32_t,KObject *);
extern pthread_mutex_t g_klock;
KEvent *kernel_create_event_pair(uint32_t *,uint32_t *);
void kernel_signal(KObject *);
''')
 (OUT/'hle.h').write_text('''#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#define LOG(...) ((void)0)
#define MAKE_RESULT(module,desc) ((uint32_t)(module)|((uint32_t)(desc)<<9))
#define HLE_CAT2(a,b) a##b
#define HLE_CAT(a,b) HLE_CAT2(a,b)
#define RT_ON_RESET(fn) static void fn(void)
''')
 (OUT/'settings.h').write_text('#pragma once\nconst char *settings_language(void);\n')
 (OUT/'title_adapter.h').write_text('#pragma once\ntypedef struct { const char *version; } TitleAdapter;\nconst TitleAdapter *title_adapter_selected(void);\n')
 shutil.copyfile(IPC,OUT/'production_ipc.c')
 text=KERNEL.read_text();start=text.index('void kernel_close_service_handle(');end=text.index('\n}\n',start)+3
 (OUT/'production_kernel_close.inc').write_text(text[start:end])
 variants={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'])]:
  binary=OUT/mode;cmd=[shutil.which('clang'),'-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter',*flags,'-I',str(OUT),str(TEST),*[str(OUT/'services'/p.name)for p in FILES],'-pthread','-o',str(binary)]
  built=subprocess.run(cmd,capture_output=True,text=True);(OUT/(mode+'-build.log')).write_text(built.stdout+built.stderr)
  if built.returncode:raise RuntimeError(built.stderr)
  run=subprocess.run([binary],capture_output=True,text=True,timeout=30);(OUT/(mode+'-run.log')).write_text(run.stdout+run.stderr)
  if run.returncode:raise RuntimeError(run.stdout+run.stderr)
  variants[mode]=json.loads(run.stdout);print(mode+': '+run.stdout.strip())
 assert pins=={p:sha(ROOT/p)for p in pins},'Source changed during verification'
 report={'state':'passed','source_sha256':pins,'variants':variants,'scope':'Actual service registry and production network/account/am handlers; synthetic IPC arguments and event kernel. Verifies offline results, no fabricated ID/token/object on failed online calls, request event completion, native applet lifecycle and ProfileSelect cancel storage. No device or game-level crash reproduction.'}
 (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
