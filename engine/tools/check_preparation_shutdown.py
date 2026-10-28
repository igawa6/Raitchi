#!/usr/bin/env python3
"""Check actual shutdown drains preparation on UI exits without worker self-waits."""
import hashlib,json,pathlib,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
SOURCE=ROOT/'ports/shared-android/runtime/nvn/vulkan/device.c'
TEST=ROOT/'tests/preparation_shutdown_test.c'
def main():
 out=ROOT/'build/preparation-shutdown';out.mkdir(parents=True,exist_ok=True)
 s=SOURCE.read_text();a=s.index('void backend_quit(void) {');b=s.index('\n}',a)+2
 (out/'preparation_shutdown_production.inc').write_text(s[a:b]+'\n')
 results={}
 for variant,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
  exe=out/variant
  p=subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror',*flags,'-I',str(out),str(TEST),'-o',str(exe)],capture_output=True,text=True,timeout=60)
  assert not p.returncode,p.stdout+p.stderr
  results[variant]=[]
  for mode in ['main','worker','no-device']:
   p=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=10);assert not p.returncode,p.stdout+p.stderr;results[variant].append(p.stdout.strip())
 report={'state':'passed','results':results,'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in [SOURCE,TEST,pathlib.Path(__file__).resolve()]},'scope':'Verbatim backend_quit with controlled thread/device and ordered hooks; main drains after renderer/recorder stop and before GPU wait/cache save, fatal worker avoids self-wait, no-device and repeated shutdown are safe. No driver/device execution.'}
 (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
