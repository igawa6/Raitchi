#!/usr/bin/env python3
"""Asset-free actual host-atomic helper tests and Android contention probe."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def sha(path):
 with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018');args=parser.parse_args()
 out=ROOT/'build/native-atomic-probe';out.mkdir(parents=True,exist_ok=True);cc=shutil.which('clang');assert cc
 tools=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin';clang=tools/'aarch64-linux-android30-clang'
 def run(command,name):
  result=subprocess.run(list(map(str,command)),text=True,capture_output=True,timeout=60);(out/name).write_text(result.stdout+result.stderr)
  if result.returncode:raise RuntimeError(str(command[0])+' failed: '+result.stdout+result.stderr)
  return result.stdout
 source=ROOT/'native/native-game-backend/atomic_step.c';header=ROOT/'native/native-game-backend/atomic_step.h';probe=ROOT/'tests/native_atomic_probe.c'
 paths=[source,header,probe,ROOT/'native/runtime/aot_cpu.h',Path(__file__).resolve()];pinned={str(p.relative_to(ROOT)):sha(p) for p in paths}
 instructions=[];cases=[];ordered=[]
 for bytes_,pair in [(1,0),(2,0),(4,0),(8,0),(8,1),(16,1)]:
  register='x' if bytes_//(2 if pair else 1)==8 else 'w';suffix='b' if bytes_==1 else 'h' if bytes_==2 else ''
  for acquire in (0,1):
   for release in (0,1):
    if pair:load=('ldaxp' if acquire else 'ldxp')+' '+register+'0,'+register+'3,[x2]';store=('stlxp' if release else 'stxp')+' w1,'+register+'0,'+register+'3,[x2]'
    else:load=('ldaxr' if acquire else 'ldxr')+suffix+' '+register+'0,[x2]';store=('stlxr' if release else 'stxr')+suffix+' w1,'+register+'0,[x2]'
    index=len(instructions);instructions.extend([load,store]);cases.append((index,bytes_,pair))
 for bytes_ in (1,2,4,8):
  register='x' if bytes_==8 else 'w';suffix='b' if bytes_==1 else 'h' if bytes_==2 else '';index=len(instructions);instructions.extend(['ldar'+suffix+' '+register+'0,[x2]','stlr'+suffix+' '+register+'0,[x2]']);ordered.append((index,bytes_))
 extra={'SP_LOAD':'ldxr x0,[sp]','SP_STORE':'stxr w1,x0,[sp]','ZR_LOAD':'ldxr xzr,[x2]','ZR_STORE':'stxr wzr,x0,[x2]','ZR_VALUE_STORE':'stxr w1,xzr,[x2]','64_LOAD':'ldaxr x0,[x2]','64_STORE':'stlxr w1,x0,[x2]','PAIR_LOAD':'ldaxp x0,x3,[x2]','PAIR_STORE':'stlxp w1,x0,x3,[x2]','CLEAR':'clrex','NOP':'nop'};indices={}
 for name,instruction in extra.items():indices[name]=len(instructions);instructions.append(instruction)
 assembly=out/'words.S';assembly.write_text('.text\n'+ '\n'.join(instructions)+'\n');obj=out/'words.o';raw=out/'words.bin';run([clang,'-c',assembly,'-o',obj],'assemble.log');run([tools/'llvm-objcopy','--only-section=.text','-O','binary',obj,raw],'extract.log');binary=raw.read_bytes();assert len(binary)==4*len(instructions);words=struct.unpack('<'+'I'*len(instructions),binary)
 text=['/* Generated asset-free AArch64 words. */','typedef struct AtomicCase {uint32_t load,store;unsigned bytes,pair;} AtomicCase;','typedef struct OrderedCase {uint32_t load,store;unsigned bytes;} OrderedCase;','#define ATOMIC_CASE_COUNT '+str(len(cases)),'static const AtomicCase atomic_cases[]={']
 text += ['{'+','.join([hex(words[index])+'u',hex(words[index+1])+'u',str(bytes_),str(pair)])+'},' for index,bytes_,pair in cases];text+=['};','#define ORDERED_CASE_COUNT '+str(len(ordered)),'static const OrderedCase ordered_cases[]={'];text+=['{'+','.join([hex(words[index])+'u',hex(words[index+1])+'u',str(bytes_)])+'},' for index,bytes_ in ordered];text+=['};']+[f'#define ATOMIC_{name} {hex(words[index])}u' for name,index in indices.items()];(out/'native_atomic_words.h').write_text('\n'.join(text)+'\n')
 tests={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  executable=out/name;run([cc,'-std=c11','-Wall','-Wextra','-Werror','-mcx16',*flags,'-DNATIVE_ATOMIC_HOST_TEST','-I',ROOT/'native','-I',out,source,probe,'-pthread','-o',executable],name+'-build.log');report=json.loads(run([executable],name+'-run.log'));assert report['state']=='passed' and report['failures']==0;tests[name]=report
 exports=out/'exports.map';exports.write_text('NATIVE_ATOMIC_PROBE_1 { global: native_atomic_probe; local: *; };\n');helper=out/'libnative_atomic_probe.so'
 run([clang,'-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-fPIC','-ffixed-x18','-fvisibility=hidden','-shared','-I',ROOT/'native','-I',out,source,probe,'-pthread','-Wl,--no-undefined','-Wl,--version-script='+str(exports),'-Wl,-z,max-page-size=16384','-Wl,-z,relro','-Wl,-z,now','-Wl,-z,noexecstack','-o',helper],'android-build.log')
 symbols=run([tools/'llvm-nm','--dynamic','--defined-only',helper],'exports.log');assert[line.split()[-1].split('@')[0] for line in symbols.splitlines()]==['native_atomic_probe'];disassembly=run([tools/'llvm-objdump','--no-show-raw-insn','-d',helper],'disassembly.txt');bad=[]
 for line in disassembly.splitlines():
  match=re.match(r'\s*[0-9a-f]+:\s+([a-z0-9.]+)\s+(.*)',line)
  if not match:continue
  op,operands=(x.lower() for x in match.groups())
  if ((op=='msr' and re.match(r'tpidr(?:ro)?_el0\b',operands)) or (re.match(r'[xw]18(?:,|$)',operands) and op not in ('str','stur','stlr','stp','stnp')) or (op in ('ldp','ldnp') and re.match(r'[^,]+,\s*[xw]18\b',operands))):bad.append((op,operands))
 assert not bad,'Physical X18/TLS write: '+repr(bad)
 config={'schema':1,'helper_sha256':sha(helper),'atomic_source_sha256':sha(source),'outer_timeout_seconds':60};(out/'config.json').write_text(json.dumps(config,indent=2)+'\n');runner=out/'run_probe.py';runner.write_text('''import ctypes,hashlib,json,sys
config=json.load(open(sys.argv[1]));helper=sys.argv[2]
with open(helper,'rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
if digest!=config['helper_sha256']:raise SystemExit('Helper hash mismatch')
lib=ctypes.CDLL(helper);probe=lib.native_atomic_probe;probe.argtypes=[ctypes.c_void_p,ctypes.c_size_t];probe.restype=ctypes.c_int
output=ctypes.create_string_buffer(4096);status=probe(output,len(output));report=json.loads(output.value);report.update({k:config[k] for k in ('helper_sha256','atomic_source_sha256')});print(json.dumps(report),flush=True);raise SystemExit(status!=0)
''')
 assert pinned=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Source changed during verification'
 record={'schema':1,'host_tests':tests,'android_cross_link_passed':True,'device_execution':'pending-coordinator','sources_sha256':pinned,'artifacts_sha256':{str(p.relative_to(ROOT)):sha(p) for p in [helper,out/'config.json',runner,out/'native_atomic_words.h']},'physical_x18_tls_writes':bad,'scope':'actual atomic helper + synthetic Cpu/value monitor + ordinary host memory; native target128 pair/contended execution pending device'};(out/'verification.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps({'host_tests':tests,'helper_sha256':sha(helper),'android_cross_link_passed':True},indent=2))
if __name__=='__main__':main()
