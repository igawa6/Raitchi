#!/usr/bin/env python3
"""Actual queue/loading transition and graphics-preparation priority integration."""
import hashlib,json,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
OUT=ROOT/'build/shader-worker-queue'
SOURCE=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c'
POLICY=ROOT/'ports/shared-android/runtime/nvn/shader_worker_policy.h'
TEST=ROOT/'tests/shader_worker_queue_test.c'
def function(text,name):
 at=text.index(name);start=text.rfind('\n',0,at)+1;brace=text.index('{',at);depth=1;end=brace+1
 while depth:
  if text[end]=='{':depth+=1
  elif text[end]=='}':depth-=1
  end+=1
 return text[start:end]+'\n'
def main():
 OUT.mkdir(parents=True,exist_ok=True);files=(SOURCE,POLICY,TEST,SOURCE.parent.parent/'shader_job_queue.c',SOURCE.parent.parent/'shader_job_queue.h',pathlib.Path(__file__).resolve());pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files};src=SOURCE.read_text();begin=src.index('static JobQueue g_jobs');end=src.index('static void *worker(',begin)
 (OUT/'worker_queue_actual.inc').write_text('#include \"nvn/shader_job_queue.c\"\n'+src[begin:end]);(OUT/'worker_run_job_actual.inc').write_text(function(src,'static void run_job_q('));(OUT/'worker_translate_actual.inc').write_text(function(src,'static void translate_program('))
 results={}
 for name,flags in (('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'])):
  exe=OUT/name;cmd=[shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',str(ROOT/'ports/shared-android/runtime'),'-I',str(OUT),str(TEST),'-pthread','-o',str(exe)]
  build=subprocess.run(cmd,capture_output=True,text=True,timeout=120);(OUT/(name+'-build.log')).write_text(build.stdout+build.stderr)
  if build.returncode:raise RuntimeError(build.stdout+build.stderr)
  runs=[]
  for i in range(5):
   run=subprocess.run([exe],capture_output=True,text=True,timeout=30);(OUT/(name+f'-run{i}.log')).write_text(run.stdout+run.stderr)
   if run.returncode:raise RuntimeError(name+f' run{i}: '+run.stdout+run.stderr)
   runs.append(json.loads(run.stdout))
  results[name]=runs
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'sources changed during test'
 report={'state':'passed','results':results,'source_sha256':pins,'scope':'Actual JobQueue/mode/run_job_q/translate_program; synthetic compiler bodies and immortal-worker cleanup; host only'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
