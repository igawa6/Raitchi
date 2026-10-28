#!/usr/bin/env python3
"""Check actual generic NVN ABI negotiation and opaque bootstrap methods; no games/device."""
import hashlib,json,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def body(text,signature):
 start=text.index(signature);at=text.index('{',start);end=at+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
def main():
 out=ROOT/'build/nvn-abi';out.mkdir(parents=True,exist_ok=True)
 source=ROOT/'ports/shared-android/runtime/nvn/objects.c';text=source.read_text()
 names=['static uint32_t device_integer(',*[f'NVN({name})' for name in ['nvnDeviceBuilderSetDefaults','nvnDeviceBuilderSetFlags','nvnDeviceBuilderGetFlags','nvnDeviceInitialize','nvnDeviceGetInteger']]]
 (out/'nvn_abi_bodies.h').write_text('\n'.join(body(text,n) for n in names)+'\n')
 profiles=[ROOT/('native/adapters/'+name+'/profile.c') for name in ['las','letsgo','echoes','wonder']]
 inputs=[source,ROOT/'native/adapters/title_adapter.h',ROOT/'tests/nvn_abi_test.c',Path(__file__).resolve(),*profiles]
 hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
 def run(command,log):
  p=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=60);(out/log).write_text(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(log+': '+p.stdout+p.stderr)
  return p.stdout.strip()
 common=['-std=c11','-Wall','-Wextra','-Werror','-I',ROOT/'native','-I',out,ROOT/'tests/nvn_abi_test.c',*profiles]
 result={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/name;run(['clang',*common,*flags,'-o',binary],name+'-compile.log');result[name]=run([binary],name+'-run.log')
 ndk=Path.home()/'android-sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang'
 run([ndk,*common,'-O2','-ffixed-x18','-Wl,--no-undefined','-o',out/'android-test'],'android-link.log')
 assert hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
 report={'state':'passed','host_tests':result,'android_link':True,'source_sha256':hashes,'scope':'Actual extracted query/builder/initialize bodies with real adapter metadata and guarded opaque objects; backend/queue/prep helpers controlled; no GPU/device/game qualification'}
 (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
