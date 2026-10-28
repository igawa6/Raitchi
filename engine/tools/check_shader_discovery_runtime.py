#!/usr/bin/env python3
"""Live portable discovery integration with current key derivation and controlled workers."""
import hashlib,json,pathlib,re,shutil,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
EXEC=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c'
HEADER=ROOT/'ports/shared-android/runtime/nvn/shader_discovery.h'
TEST=ROOT/'tests/shader_discovery_runtime_test.c'
PARALLEL=ROOT/'tests/shader_discovery_parallel_test.inc'
HASH=ROOT/'ports/shared-android/runtime/shared_hash.cpp'
OUT=ROOT/'build/shader-discovery-runtime'
CASES=('graphics','depth','compute','compute_limit','compute_policy','fail_program','fail_module','fail_pipeline','fail_compute','cancel','queue','concurrent_finish','queue_failure','queue_caps','table_full','retained_cap','strict_reuse','strict_roundtrip','unproved')
REAL_CASES=('real_serial','real_parallel','real_one','real_cancel','real_fail_program','real_fail_module','real_fail_pipeline','real_fail_compute','real_budget','real_append','real_bounds')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def clean(s):
 return re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',lambda m:''.join('\n' if c=='\n' else ' ' for c in m[0]),s)
def function(s,name):
 c=clean(s)
 for m in re.finditer(r'\b'+re.escape(name)+r'\s*\(',c):
  i=m.end();depth=1
  while depth:depth+=(c[i]=='(')-(c[i]==')');i+=1
  while c[i].isspace():i+=1
  if c[i]!='{':continue
  start=c.rfind('\n',0,m.start())+1;end=i+1;depth=1
  while depth:depth+=(c[end]=='{')-(c[end]=='}');end+=1
  return s[start:end]+'\n'
 raise ValueError(name)
def typedef(s,name):
 end=s.index('} '+name+';')+len('} '+name+';');start=s.rindex('typedef struct {',0,end);return s[start:end]+'\n'
def main():
 OUT.mkdir(parents=True,exist_ok=True);s=EXEC.read_text()
 types='typedef uint64_t VkPipeline;\n'+typedef(s,'VtxLayout')+typedef(s,'PipeKey')+typedef(s,'PipeIn')
 types+=re.search(r'typedef struct \{ PipeKey k; VkPipeline pipe; _Atomic int state; \} Pipe;',s)[0]+'\n'
 types+='typedef struct CsProg CsProg;\n'+re.search(r'struct CsProg \{[^\n]+\};',s)[0]+'\n'
 types+=re.search(r'typedef struct \{[^\n]+\} HsCsHeader;',s)[0]+'\n'+re.search(r'typedef struct \{[^\n]+\} HsCsTranslate;',s)[0]+'\n'
 types+=re.search(r'typedef struct \{[^\n]+\} HsProgHeader;',s)[0]+'\n'+re.search(r'typedef struct \{ uint32_t type, size; \} HsRec;',s)[0]+'\n'+typedef(s,'HsHeader')
 (OUT/'discovery_runtime_types.inc').write_text(types)
 qstart=s.index('static JobQueue g_jobs');qend=s.index('static int shader_workers',qstart)
 (OUT/'discovery_runtime_queue.inc').write_text('#include \"nvn/shader_job_queue.c\"\n'+s[qstart:qend]+function(s,'queue_push'))
 prep=ROOT/'work/shared-android/runtime/nvn/shader_prep.c'
 functions=[function(prep.read_text(),'shader_code_hash')]+[function(s,n)for n in ('hash_words','prog_content_hash','prog_code','hs_add_locked','hs_add','hs_take','hs_pipe_take','hs_pipe_put','pipe_key_hash','hs_cs_size_valid','hs_cs_content_key','hs_program_size_valid','hs_cs_recorded_clear_locked','hs_cs_recorded_find_locked','hs_cs_recorded_add_locked','hs_cache_open','hs_prepare_locked','hs_write','hs_stage_replayable','hs_program_replayable','hs_record_program_closure','hs_record_program','hs_record_compute','hs_record_compute_once','hs_record_pipe','pipe_key_fill','prepare_pipe_key','hs_cs_local_supported','hs_cs_replay','hs_cs_state','translate_program_now')]
 start=s.index('// ---- durable guest discovery:');end=s.index('// ================================================================== graphics pipeline library probe',start)
 functions.append(s[start:end]);functions.append(function(s,'headstart_thread').replace('mmap(', 'tracked_mmap('));functions.extend(function(s,n)for n in ('vk_headstart_prepare','vk_headstart_finish'))
 (OUT/'discovery_runtime_functions.inc').write_text('\n'.join(functions))
 # Source joins must remain on original inputs and canonical current interfaces.
 capture=s[s.index('static void rec_inputs_from_state('):s.index('static void debug_draw(',s.index('static void rec_inputs_from_state('))]
 assert 'prepare_pipe_key(&reproduced, pr, &recipe.rec);'in capture
 assert 'if (!memcmp(&reproduced, &k, sizeof k)) discovery_record(&recipe, pr->chash);'in capture
 assert 'fkey.vs_out_mask = p->vi->out_mask;'in function(s,'translate_program_now')
 compute=s[s.index('static CsProg *compute_program('):s.index('// ================================================================== shader preparation',s.index('static CsProg *compute_program('))]
 assert compute.index('discovery_record(&recipe, ch);')<compute.index('PREP_GET(prep_cs,')
 paths=[EXEC,HEADER,TEST,PARALLEL,HASH,prep,ROOT/'ports/shared-android/runtime/nvn/shader_job_queue.c',ROOT/'ports/shared-android/runtime/nvn/shader_job_queue.h',ROOT/'ports/shared-android/runtime/nvn/maxwell/shader.h',pathlib.Path(__file__).resolve()]
 pins={str(p.relative_to(ROOT)):sha(p)for p in paths}
 include=[OUT,ROOT/'ports/shared-android/runtime/nvn',ROOT/'work/shared-android/runtime/nvn',ROOT/'ports/shared-android/runtime']
 def run(cmd,name,cwd=None):
  r=subprocess.run(list(map(str,cmd)),cwd=cwd,capture_output=True,text=True,timeout=60);(OUT/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'])]:
  obj=OUT/(mode+'-test.o');hobj=OUT/(mode+'-hash.o');exe=OUT/mode
  run([shutil.which('clang'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,*sum((['-I',p]for p in include),[]),TEST,'-c','-o',obj],mode+'-build.log')
  run([shutil.which('clang++'),'-std=c++17',*flags,'-I',ROOT/'sources/las-android/runtime','-I',ROOT/'ports/shared-android/runtime','-I',ROOT/'native','-c',HASH,'-o',hobj],mode+'-hash-build.log')
  run([shutil.which('clang++'),*flags,obj,hobj,'-pthread','-o',exe],mode+'-link.log')
  reports[mode]={}
  for case in CASES:
   with tempfile.TemporaryDirectory(prefix='shader-runtime-')as d:reports[mode][case]=json.loads(run([exe,case,d],mode+'-'+case+'.log'))
  real_obj=OUT/(mode+'-real-test.o');real_exe=OUT/(mode+'-real')
  run([shutil.which('clang'),'-std=gnu11','-DREAL_WORKERS=1','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,*sum((['-I',p]for p in include),[]),TEST,'-c','-o',real_obj],mode+'-real-build.log')
  run([shutil.which('clang++'),*flags,real_obj,hobj,'-pthread','-o',real_exe],mode+'-real-link.log')
  for case in REAL_CASES:
   with tempfile.TemporaryDirectory(prefix='shader-parallel-')as d:reports[mode][case]=json.loads(run([real_exe,case,d],mode+'-'+case+'.log'))
  serial=reports[mode]['real_serial'];parallel=reports[mode]['real_parallel']
  assert serial['max_builders']==1 and serial['pipeline_overlap']==0
  assert parallel['max_builders']>=2 and parallel['pipeline_overlap']>0
  assert reports[mode]['real_one']['max_builders']==1
  print(mode+': '+str(len(CASES))+' integration and '+str(len(REAL_CASES))+' actual queue scheduling scenarios passed; serial '+str(serial['elapsed_ms'])+' ms, batch '+str(parallel['elapsed_ms'])+' ms (synthetic only)')
 assert pins=={p:sha(ROOT/p)for p in pins},'Sources changed during tests'
 report={'state':'passed','source_sha256':pins,'variants':reports,'scope':'Actual strict record writers/cache validation/headstart reader plus portable discovery, current key/interface derivation and readiness. Real files/codec/SHA; controlled identity generation, translator/GPU workers. Fresh portable writeback then process-local reset/actual strict reload must produce zero extra portable jobs/writes. No actual driver/GPU/game payload.'}
 (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
