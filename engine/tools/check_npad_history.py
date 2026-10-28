#!/usr/bin/env python3
"""Actual plural Npad ABI/provider with controlled host events and LAS reentry."""
import hashlib,json,pathlib,re,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];RUNTIME=ROOT/'ports/shared-android/runtime';SOURCE=RUNTIME/'hle/input.c';OUT=ROOT/'build/npad-history'
def function(s,signature):
 start=s.index(signature);a=s.index('{',start);depth=1;i=a+1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]+'\n'
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);s=SOURCE.read_text();a=s.index('static _Atomic uint64_t sampling;');b=s.index('static bool host_test;',a);body=s[a:b]
 a=s.index('enum {');b=s.index('};',a)+2;body+=s[a:b]+'\n'
 body+=function(s,'static int32_t clamp_axis(')+function(s,'void input_reset(')+function(s,'RT_ON_RESET(input_reboot_reset)')
 a=s.index('static void fill_state(');b=s.index('// Selected title policy:',a);body+=s[a:b]
 aliases=re.findall(r'HLE_FUNC\("([^"\n]*GetNpadStates[^"\n]*)", (hid_states_\w+)\)',body);assert len(aliases)==5
 assert 'Cpu saved' not in body and 'Cpu copy' not in body
 (OUT/'npad_input_production.inc').write_text(body)
 paths=[SOURCE,RUNTIME/'hle/npad_history.c',RUNTIME/'hle/npad_history.h',ROOT/'tests/npad_history_test.c',ROOT/'native/runtime/aot_cpu.h',pathlib.Path(__file__)];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths};results={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/name;run([shutil.which('clang'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,'-I',ROOT/'native','-I',RUNTIME,'-I',OUT,ROOT/'tests/npad_history_test.c',RUNTIME/'hle/npad_history.c','-pthread','-o',exe]);results[name]=run([exe])
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},'Sources changed during checks'
 report={'state':'passed','results':results,'plural_aliases':aliases,'source_sha256':pins,'scope':'Actual fill_state, plural wrappers/history/epoch and reset bodies with actual Cpu ABI; controlled input and guest-helper callbacks. Tests40B stride, unaligned outputs/id refs, stable newest-first16, warmup zeroing/press-release edges, limits/null/negative/largecount, style/ID isolation, reset interleaving and sameCpu reentry. No SDL/device/gameplay claims.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
