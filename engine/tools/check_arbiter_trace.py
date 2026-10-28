#!/usr/bin/env python3
"""Exercise actual bounded diagnostics and arbiter queue operations on host threads."""
import argparse,hashlib,json,pathlib,re,shlex,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--android-object',action='store_true');options=parser.parse_args()
 out=ROOT/'build/arbiter-trace';out.mkdir(parents=True,exist_ok=True)
 source=ROOT/'ports/shared-android/runtime/kernel.c';text=source.read_text()
 def function(sig):
  a=text.index(sig);i=text.index('{',a)+1;depth=1
  while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
  return text[a:i]+'\n'
 fragment=text[text.index('// BEGIN bounded arbiter diagnostics'):text.index('// END bounded arbiter diagnostics')]
 for sig in ['static void wake(', 'static void deadline(', 'static bool kwait(', 'static void waitlist_add(', 'static void waitlist_remove(', 'static uint32_t block_current(']:fragment+=function(sig)
 fragment+='enum { ARB_WAIT_LESS, ARB_DEC_WAIT_LESS, ARB_WAIT_EQ };\nenum { ARB_SIGNAL, ARB_SIGNAL_INC_EQ, ARB_SIGNAL_MOD_EQ };\n'
 for sig in ['static uint32_t svc_wait_for_address(', 'static int arbiter_wake(', 'static bool arbiter_update(', 'static uint32_t svc_signal_to_address(']:fragment+=function(sig)
 (out/'arbiter_trace_production.h').write_text(fragment)
 for sig in ['void kernel_reset(', 'void kernel_init(']:assert 'arb_trace_reset();' in function(sig)
 sha=hashlib.sha256(source.read_bytes()).hexdigest();reports={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/mode
  for step,args in [('compile',['clang','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',str(out),str(ROOT/'tests/arbiter_trace_test.c'),'-pthread','-o',str(binary)]),('run',[str(binary)])]:
   r=subprocess.run(args,capture_output=True,text=True,timeout=60);(out/f'{mode}-{step}.log').write_text(r.stdout+r.stderr)
   if r.returncode:raise RuntimeError(r.stdout+r.stderr)
   if step=='run':reports[mode]=json.loads(r.stdout)
 artifact={}
 if options.android_object:
  # Compile the actual production TU, which has no C consumer reading records.
  # Test-only reads above therefore cannot hide dead-store elimination here.
  row=next(x for x in json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text()) if x['file'].endswith('/runtime/kernel.c'))
  original=row.get('arguments') or shlex.split(row['command']);args=[];i=0
  while i<len(original):
   if original[i] in ('-o','-MF','-MT','-MQ'):i+=2;continue
   if original[i] in ('-MD','-MMD','-MP'):i+=1;continue
   args.append(original[i]);i+=1
  overlay=out/'kernel-overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':row['file'],'external-contents':str(source),'use-external-name':False}]}))
  obj=out/'kernel-android.o';r=subprocess.run([*args,'-ivfsoverlay',str(overlay),'-O3','-o',str(obj)],cwd=row['directory'],capture_output=True,text=True,timeout=60)
  (out/'android-object-compile.log').write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(r.stdout+r.stderr)
  bindir=pathlib.Path(args[0]).parent
  nm=subprocess.run([str(bindir/'llvm-nm'),'-S',str(obj)],capture_output=True,text=True,check=True).stdout
  (out/'android-object-nm.txt').write_text(nm)
  symbols={m[3]:int(m[1],16) for m in re.finditer(r'^[0-9a-f]+\s+([0-9a-f]+)\s+([bBdD])\s+(arb_trace\w*)$',nm,re.M)}
  assert symbols.get('arb_trace')==8192*104,symbols
  for name in ['count','overflow','polls','filtered','filter','from','to','config_error']:assert symbols.get('arb_trace_'+name)==8,symbols
  disasm=subprocess.run([str(bindir/'llvm-objdump'),'-dr','--disassemble-symbols=arb_trace_record',str(obj)],capture_output=True,text=True,check=True).stdout
  (out/'android-record-disassembly.txt').write_text(disasm)
  assert 'arb_trace' in disasm and re.search(r'\b(?:str|stp)\b',disasm), 'record stores not emitted'
  artifact={'actual_android_production_TU_O3':True,'debugger_symbols':symbols,'record_stores_emitted':True,'object_sha256':hashlib.sha256(obj.read_bytes()).hexdigest()}
 assert sha==hashlib.sha256(source.read_bytes()).hexdigest()
 report={'state':'passed','host':reports,'compiled_artifact':artifact,'kernel_sha256':sha,'scope':'Actual diagnostic helper and arbiter wait/signal bodies; optional actual production Android TU visibility check without test consumers. No device/full native build. Fault-safe inaccessible-word read, signal-before-wait, queued wake, invalid-state/enum, finite/poll timeout, exact filter, concurrent no-overwrite capacity and reset.'}
 (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
