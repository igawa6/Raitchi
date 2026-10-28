#!/usr/bin/env python3
"""Combined actual continuous context/image/signals probe; no ADB or APK action."""
import argparse,hashlib,json,re,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):
 with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018');a=p.parse_args()
 out=ROOT/'build/continuous-signals-probe';out.mkdir(parents=True,exist_ok=True);tools=a.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin';android=tools/'aarch64-linux-android30-clang';runtime=ROOT/'ports/shared-android/runtime'
 def run(args,log):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60);(out/log).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(log+': '+r.stdout+r.stderr)
  return r.stdout
 assembly='''.text
.global fault,dirty,site,spin,spin_site
fault:
 ldr x0,[x2]
dirty:
 str x0,[x3]
 add x18,x18,#9
site:
 svc #0x10
spin:
 ldr w0,[x4]
 cbnz w0,spin
spin_site:
 svc #0x1e
'''
 (out/'program.S').write_text(assembly);run([android,'-c',out/'program.S','-o',out/'program.o'],'assemble.log');run([android,'-nostdlib','-static','-Wl,--build-id=none','-Wl,-Ttext=0','-Wl,-e,fault',out/'program.o','-o',out/'program.elf'],'program-link.log');run([tools/'llvm-objcopy','--only-section=.text','-O','binary',out/'program.elf',out/'program.bin'],'extract.log')
 data=(out/'program.bin').read_bytes();header=['static const unsigned char program_bytes[]={'+','.join(str(b) for b in data)+'};']
 for line in run([tools/'llvm-nm','--defined-only',out/'program.elf'],'symbols.log').splitlines():
  parts=line.split()
  if len(parts)==3 and parts[2] in ('fault','dirty','site','spin','spin_site'):header.append('#define PC_'+parts[2].upper()+' '+str(int(parts[0],16)))
 (out/'continuous_signals_words.h').write_text('\n'.join(header)+'\n')
 sources=[ROOT/'tests/continuous_signals_probe.c',runtime/'continuous_context.c',runtime/'continuous_signals.c',runtime/'continuous_image.c',ROOT/'native/continuous-nce/patch_plan.c'];paths=sources+[runtime/'continuous_context.S',runtime/'continuous_context.h',runtime/'continuous_signals.h',runtime/'continuous_image.h',ROOT/'native/runtime/aot_cpu.h',ROOT/'native/continuous-nce/patch_plan.h',ROOT/'native/runtime/session_layout.h',Path(__file__).resolve()];pins={str(p.relative_to(ROOT)):sha(p) for p in paths}
 common=['-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsigned-char','-fno-strict-aliasing','-fstack-protector-strong','-I',ROOT/'native','-I',runtime,'-I',ROOT/'sources/las-android/runtime','-I',out]
 tests={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/('host-'+name);run([shutil.which('clang'),*common,*flags,'-DCONTINUOUS_SIGNALS_HOST_TEST',*sources,'-pthread','-ldl','-o',binary],name+'-build.log');stdout=run([binary],name+'-run.log');assert 'failures=0' in stdout;tests[name]={'passed':True,'checks':int(re.search(r'checks=(\d+)',stdout)[1]),'scope':'real image preparation and signal installer reentry; explicit nonexecuting context stand-ins on host'}
 exports=out/'exports.map';exports.write_text('CONTINUOUS_SIGNALS_PROBE_1 { global: continuous_signals_probe; local: *; };\n');helper=out/'libcontinuous_signals_probe.so'
 run([android,*common,'-O2','-g','-fPIC','-ffixed-x18','-fvisibility=hidden','-shared',*sources,runtime/'continuous_context.S','-pthread','-ldl','-Wl,--no-undefined','-Wl,-Bsymbolic','-Wl,--version-script='+str(exports),'-Wl,-z,max-page-size=16384','-Wl,-z,relro','-Wl,-z,now','-Wl,-z,noexecstack','-o',helper],'android-build.log')
 symbols=run([tools/'llvm-nm','--dynamic','--defined-only',helper],'exports.log');assert [x.split()[-1].split('@')[0] for x in symbols.splitlines()]==['continuous_signals_probe']
 dis=run([tools/'llvm-objdump','-d',helper],'disassembly.txt');front=dis.split('<continuous_context_transition_end>:',1)[1].split('\n\n',1)[0];assert 'tpidr_el0' in front.lower();(out/'signal-front.txt').write_text(front)
 config={'schema':1,'helper_sha256':sha(helper),'outer_timeout_seconds':30};(out/'config.json').write_text(json.dumps(config,indent=2)+'\n')
 runner=out/'run_probe.py';runner.write_text('''import ctypes,hashlib,json,os,sys
config=json.load(open(sys.argv[1]));path=sys.argv[2]
with open(path,'rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
if digest!=config['helper_sha256']:raise SystemExit('Helper hash mismatch')
trace=os.path.join(os.path.dirname(os.path.abspath(path)),'fault-trace.bin');os.environ['LARECOMP_NCE_FAULT_TRACE']=trace
lib=ctypes.CDLL(path);fn=lib.continuous_signals_probe;fn.argtypes=[ctypes.c_void_p,ctypes.c_size_t];fn.restype=ctypes.c_int
buf=ctypes.create_string_buffer(8192);status=fn(buf,len(buf));report=json.loads(buf.value);report['fault_trace_bytes']=os.path.getsize(trace) if os.path.exists(trace) else -1;report['helper_sha256']=digest;print(json.dumps(report),flush=True);raise SystemExit(status!=0 or report['state']!='passed')
''')
 assert pins=={str(p.relative_to(ROOT)):sha(p) for p in paths},'Runtime changed during probe verification; rerun after source freeze'
 (out/'verification.json').write_text(json.dumps({'schema':1,'host_tests':tests,'android_cross_link_passed':True,'helper_sha256':sha(helper),'source_sha256':pins,'artifacts_sha256':{str(p.relative_to(ROOT)):sha(p) for p in [helper,runner,out/'config.json',out/'continuous_signals_words.h']},'scope':'actual context+image+signals modules, synthetic Cpu identity via pthread TLS, no ART process or real KThread/game qualification','device_execution':'pending-coordinator'},indent=2)+'\n');print(json.dumps({'host_tests':tests,'helper_sha256':sha(helper),'build_directory':str(out)},indent=2))
if __name__=='__main__':main()
