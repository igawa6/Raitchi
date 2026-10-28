#!/usr/bin/env python3
"""Build the asset-free continuous-native context/TLS/signal diagnostic, no device actions."""
import argparse,hashlib,json,re,shutil,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(path):
 with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018');a=p.parse_args()
 out=ROOT/'build/continuous-context-probe';out.mkdir(parents=True,exist_ok=True)
 tools=a.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin';cc=tools/'aarch64-linux-android30-clang'
 def run(args,name):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(r.stdout+r.stderr)
  return r.stdout
 asm=''' .text
 .global start,loop,site1,stage2,after_bl,site2,leaf,fault,spin,spin_site
start:
loop:
 add x1,x1,#3
 subs x0,x0,#1
 b.ne loop
 eor v0.16b,v0.16b,v31.16b
 add x18,x18,#7
 add x19,x19,#11
 fdiv s3,s4,s5
 stp x18,x19,[sp,#-32]!
 ldp x20,x21,[sp]
 add sp,sp,#32
site1:
 svc #0x10
stage2:
 add x0,x0,#9
 add x18,x18,#5
 bl leaf
after_bl:
 b site2
leaf:
 str x30,[sp,#-16]!
 add x17,x17,#19
 ldr x30,[sp],#16
 ret
site2:
 svc #0x25
fault:
 ldr x0,[x2]
 b site2
spin:
 ldr w3,[x2]
 cbnz w3,spin
spin_site:
 svc #0x1e
'''
 (out/'program.S').write_text(asm);run([cc,'-c',out/'program.S','-o',out/'program.o'],'assemble.log')
 run([cc,'-nostdlib','-static','-Wl,--build-id=none','-Wl,-Ttext=0','-Wl,-e,start',out/'program.o','-o',out/'program.elf'],'program-link.log')
 run([tools/'llvm-objcopy','--only-section=.text','-O','binary',out/'program.elf',out/'program.bin'],'extract.log')
 raw=(out/'program.bin').read_bytes();symbols=run([tools/'llvm-nm','--defined-only',out/'program.elf'],'symbols.log')
 header=['/* Asset-free native loop/call programs. */','static const unsigned char program_bytes[]={'+','.join(str(x) for x in raw)+'};']
 for line in symbols.splitlines():
  parts=line.split()
  if len(parts)==3 and parts[2] in ('start','loop','site1','stage2','after_bl','site2','leaf','fault','spin','spin_site'):header.append('#define PC_'+parts[2].upper()+' '+str(int(parts[0],16)))
 (out/'continuous_program.h').write_text('\n'.join(header)+'\n')
 paths=[ROOT/'ports/shared-android/runtime'/('continuous_context.'+s) for s in ('c','h','S')]+[ROOT/'tests/continuous_context_probe.c',Path(__file__).resolve(),ROOT/'native/continuous-nce/patch_plan.c',ROOT/'native/continuous-nce/patch_plan.h',ROOT/'native/runtime/aot_cpu.h']
 pins={str(x.relative_to(ROOT)):sha(x) for x in paths};common=['-std=c11','-Wall','-Wextra','-Werror','-I',ROOT/'native','-I',ROOT/'ports/shared-android/runtime','-I',out]
 tests={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/('host-'+name);run([shutil.which('clang'),*common,*flags,'-DCONTINUOUS_CONTEXT_HOST_TEST',ROOT/'tests/continuous_context_probe.c',ROOT/'ports/shared-android/runtime/continuous_context.c',ROOT/'native/continuous-nce/patch_plan.c','-o',binary],name+'-build.log');stdout=run([binary],name+'-run.log');tests[name]={'passed':True,'assertions':int(re.search(r'assertions=(\d+)',stdout)[1]),'scope':'actual context validation/layout and patch emission; no native execution on host'}
 exports=out/'exports.map';exports.write_text('CONTINUOUS_CONTEXT_1 {global: continuous_context_probe; local:*;};\n');helper=out/'libcontinuous_context_probe.so'
 run([cc,*common,'-O2','-g','-fPIC','-ffixed-x18','-fvisibility=hidden','-shared',ROOT/'tests/continuous_context_probe.c',ROOT/'ports/shared-android/runtime/continuous_context.c',ROOT/'ports/shared-android/runtime/continuous_context.S',ROOT/'native/continuous-nce/patch_plan.c','-pthread','-Wl,--no-undefined','-Wl,--version-script='+str(exports),'-Wl,-z,max-page-size=16384','-Wl,-z,relro','-Wl,-z,now','-Wl,-z,noexecstack','-o',helper],'android-build.log')
 symbols=run([tools/'llvm-nm','--dynamic','--defined-only',helper],'exports.log');assert [line.split()[-1].split('@')[0] for line in symbols.splitlines()]==['continuous_context_probe']
 run([tools/'llvm-objdump','-d',helper],'disassembly.txt')
 config={'schema':1,'helper_sha256':sha(helper),'outer_timeout_seconds':30};(out/'config.json').write_text(json.dumps(config,indent=2)+'\n')
 runner=out/'run_probe.py';runner.write_text('''import ctypes,hashlib,json,sys
config=json.load(open(sys.argv[1]));path=sys.argv[2]
with open(path,'rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
if digest!=config['helper_sha256']:raise SystemExit('Helper hash mismatch')
lib=ctypes.CDLL(path);fn=lib.continuous_context_probe;fn.argtypes=[ctypes.c_void_p,ctypes.c_size_t];fn.restype=ctypes.c_int
out=ctypes.create_string_buffer(8192);status=fn(out,len(out));report=json.loads(out.value);report['helper_sha256']=digest;print(json.dumps(report),flush=True);raise SystemExit(status!=0)
''')
 assert pins=={str(x.relative_to(ROOT)):sha(x) for x in paths},'Sources changed during verification'
 (out/'verification.json').write_text(json.dumps({'schema':1,'host_tests':tests,'android_cross_link_passed':True,'helper_sha256':sha(helper),'source_sha256':pins,'artifacts_sha256':{str(x.relative_to(ROOT)):sha(x) for x in [helper,out/'config.json',runner,out/'continuous_program.h']},'scope':'synthetic Cpu/guarded guest and host-alt stacks, original asset-free ARM64 loops, no real KThread/game claim','device_execution':'pending-coordinator'},indent=2)+'\n');print(json.dumps({'host_tests':tests,'helper_sha256':sha(helper),'build_directory':str(out)},indent=2))
if __name__=='__main__':main()
