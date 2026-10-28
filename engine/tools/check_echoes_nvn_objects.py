#!/usr/bin/env python3
"""Actual NVN object-body canary/conversion checks plus full target TU compile, no device."""
import hashlib,json,re,shutil,subprocess,shlex
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def function(text,marker):
 start=text.index(marker);opening=text.index('{',start);depth=0
 for match in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]',text[opening:],re.S):
  token=match[0]
  if token=='{':depth+=1
  elif token=='}':
   depth-=1
   if not depth:return text[start:opening+match.end()]
 raise RuntimeError('Unterminated function')
def block(text,end,start='typedef struct'):
 stop=text.index(end)+len(end);return text[text.rfind(start,0,stop):stop]
def main():
 out=ROOT/'build/echoes-nvn-objects';out.mkdir(parents=True,exist_ok=True);source=ROOT/'ports/shared-android/runtime/nvn/objects.c';header=ROOT/'ports/shared-android/runtime/nvn/nvn.h'
 if not header.is_file():header=ROOT/'sources/las-android/runtime/nvn/nvn.h'
 text=source.read_text();h=header.read_text();test=ROOT/'tests/echoes_nvn_objects_test.c';paths=[source,header,test,Path(__file__).resolve(),ROOT/'native/runtime/aot_cpu.h',ROOT/'sources/las-android/runtime/nvn/tegra.h'];pins={str(p.relative_to(ROOT)):sha(p) for p in paths}
 pre='''#include <stdlib.h>\n#include <string.h>\n#include "nvn/tegra.h"\n#define NVN(name) static void n_cpu_##name(Cpu*c)\n#define A(i) (c->x[(i)])\n#define AP(i) ((void*)(uintptr_t)c->x[(i)])\n#define AF(i) (*(float*)&c->v[(i)])\n#define RET(v) (c->x[0]=(uint64_t)(v))\n#define RETF(f) do{c->v[0]=0;*(float*)&c->v[0]=(f);}while(0)\n#define HOST(T,g) ((T*)(uintptr_t)*(uint64_t*)(uintptr_t)(g))\n#define SET_HOST(g,p) (*(uint64_t*)(uintptr_t)(g)=(uintptr_t)(p))\n#define G(T,b,o) (*(T*)((uint8_t*)(uintptr_t)(b)+(o)))\n#define TB(T,o) G(T,A(0),o)\n#define TEX HOST(NvnTexture,A(0))\nstatic void *queued_free;static void rq_free(void*p){queued_free=p;}\n'''
 types='typedef struct NvnPool NvnPool;\n'+'\n'.join([block(h,'} NvnSamplerDesc;','typedef struct {'),block(h,'} NvnDescPool;','typedef struct NvnDescPool'),block(h,'} NvnTexture;','typedef struct NvnTexture')]);start=text.index('typedef struct {',text.index('// SamplerBuilder (guest memory):'));end=text.index('// ================================================================== descriptor pools',start)
 body=text[start:end]+function(text,'NVN(nvnSamplerPoolRegisterSamplerBuilder)')+'\n'+function(text,'static uint64_t zcull_size(')+'\n'+function(text,'NVN(nvnTextureBuilderGetZCullStorageSize)')+'\n'+function(text,'NVN(nvnTextureGetZCullStorageSize)')
 (out/'echoes_nvn_object_bodies.h').write_text(pre+types+'\n'+body+'\n')
 def run(args,name,**kwargs):
  p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60,**kwargs);(out/name).write_text(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(name+': '+p.stdout+p.stderr)
  return p.stdout
 common=['-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fno-strict-aliasing','-I',ROOT/'native','-I',ROOT/'sources/las-android/runtime','-I',out];reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/name;run(['clang',*common,*flags,test,'-o',binary],name+'-compile.log');stdout=run([binary],name+'-run.log');reports[name]={'passed':True,'stdout':stdout.strip()}
 ndk=Path.home()/'android-sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang';run([ndk,*common,'-O2','-ffixed-x18',test,'-Wl,--no-undefined','-o',out/'target-test'],'target-link.log')
 # Snapshot a coherent header tree, then compile the actual whole objects.c with
 # the cached production flags. This avoids mixing two pragma-once header roots.
 overlay=out/'production/runtime'
 for root in [ROOT/'work/shared-android/runtime',ROOT/'ports/shared-android/runtime']:
  for p in root.rglob('*'):
   if p.is_file() and p.suffix in ('.h','.inc'):
    target=overlay/p.relative_to(root);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
 target=overlay/'nvn/objects.c';target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(row for row in rows if row['file'].endswith('/runtime/nvn/objects.c'));args=row.get('arguments') or shlex.split(row['command']);command=[args[0],'-I'+str(overlay),'-I'+str(ROOT/'native')];i=1
 while i<len(args):
  if args[i] in ('-o','-MF','-MT','-MQ'):i+=2;continue
  if args[i] in ('-MD','-MMD','-MP'):i+=1;continue
  arg=args[i];arg=arg.replace(str(ROOT/'work/shared-android/runtime'),str(overlay))
  if args[i]==row['file']:arg=str(target)
  command.append(arg);i+=1
 command.extend(['-o',str(out/'objects.production.o')]);run(command,'production-target-compile.log',cwd=row['directory'])
 assert pins=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Sources changed during tests'
 report={'schema':1,'state':'passed','host_tests':reports,'android_body_link_passed':True,'whole_production_objects_target_compile_passed':True,'source_sha256':pins,'scope':'actual compactbuilder/getters/initialize/poolconversion/queued-finalize/Zcull bodies and current host types; guarded synthetic guest64B object; no GPU/device/game execution'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
