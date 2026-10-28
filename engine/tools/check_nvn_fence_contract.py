#!/usr/bin/env python3
"""Actual fence publication helpers/NVN wrappers with controlled queue and GPU completion."""
import hashlib,json,re,shlex,shutil,subprocess
from pathlib import Path
from check_storage_image_views import function,struct
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def wrapper(text,name):
 a=text.index('NVN('+name+')');b=text.index('{',a)+1;depth=1;c=b
 while depth:depth+=(text[c]=='{')-(text[c]=='}');c+=1
 return text[a:c]
def main():
 out=ROOT/'build/nvn-fence-contract';out.mkdir(parents=True,exist_ok=True);runtime=ROOT/'ports/shared-android/runtime';rq=runtime/'nvn/rqueue.c';obj=runtime/'nvn/objects.c';header=runtime/'nvn/rqueue.h';nvn=runtime/'nvn/nvn.h';test=ROOT/'tests/nvn_fence_contract_test.c';paths=[rq,obj,header,nvn,test,Path(__file__)];pins={str(p.relative_to(ROOT)):sha(p)for p in paths};r=rq.read_text();o=obj.read_text();exec_source=runtime/'nvn/vulkan/exec.c';e=exec_source.read_text();pins[str(exec_source.relative_to(ROOT))]=sha(exec_source)
 types='typedef struct NvnTexture NvnTexture;\n'+r[r.index('enum { IT_EXEC,'):r.index(';',r.index('enum { IT_EXEC,'))+1]+'\n'+struct(r,'typedef struct {\n    int kind;','} Item;')+'\n'+struct(nvn.read_text(),'typedef struct NvnSync {','} NvnSync;')+'\n';(out/'production_fence_types.inc').write_text(types)
 (out/'production_backend_execute.inc').write_text(function(e,'backend_execute')+'\n')
 (out/'production_item_completion.inc').write_text(function(r,'rq_item_completed_seq')+'\n');(out/'production_queue_fence.inc').write_text(function(r,'rq_fence_seq')+'\n')
 (out/'production_sync.inc').write_text('\n'.join([function(o,'sync_of'),function(o,'sync_publish'),wrapper(o,'nvnQueueFenceSync'),function(o,'nvn_sync_set_seq'),wrapper(o,'nvnSyncWait')])+'\n')
 assert '.fenced' not in function(r,'rq_submit') and 'rq_item_completed_seq(&it)' in r[r.index('static void *run(void *arg) {'):]
 assert 'lag = e && atoi(e) != 0;' in o and 'unsafe research override' in o
 def run(cmd,log,cwd=ROOT):
  p=subprocess.run(list(map(str,cmd)),cwd=cwd,capture_output=True,text=True,timeout=90);(out/log).write_text(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(log+': '+p.stdout+p.stderr)
  return p.stdout.strip()
 reports={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/mode;run([shutil.which('clang'),'-std=c11','-D_GNU_SOURCE','-DNDEBUG','-Wall','-Wextra','-Werror',*flags,'-pthread','-I',out,test,'-o',binary],mode+'-build.log');reports[mode]=json.loads(run([binary],mode+'-run.log'))
 old=out/'rqueue-before.c';prior_destroy_failed=None;prior_ordinary_passed=None
 if old.exists():
  prior=out/'prior';prior.mkdir(exist_ok=True)
  expr=re.search(r'uint64_t gs = ([^,;]+), c1 =',old.read_text()).group(1)
  assert expr=='renderer_issued_seq()'
  for name in ['production_fence_types.inc','production_sync.inc','production_backend_execute.inc']:(prior/name).write_bytes((out/name).read_bytes())
  (prior/'production_queue_fence.inc').write_text(function(old.read_text(),'rq_fence_seq')+'\n')
  (prior/'production_item_completion.inc').write_text('static uint64_t rq_item_completed_seq(const Item *it){(void)it;return '+expr+';}\n')
  binary=prior/'test';run([shutil.which('clang'),'-std=c11','-D_GNU_SOURCE','-DNDEBUG','-O2','-pthread','-I',prior,test,'-o',binary],'prior-build.log')
  ordinary=run([binary,'--ordinary-only'],'prior-ordinary-run.log');prior_ordinary_passed=json.loads(ordinary)['state']=='passed';assert prior_ordinary_passed
  p=subprocess.run([str(binary),'--destroy-contract'],capture_output=True,text=True,timeout=10);(out/'prior-destroy-run.log').write_text(p.stdout+p.stderr);prior_destroy_failed=p.returncode!=0;assert prior_destroy_failed,'Original queue fence must not complete the controlled pending destruction-tail model'
 rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime'/p.relative_to(runtime)),'external-contents':str(p)}for p in [rq,obj,header,nvn]]}))
 for source in [rq,obj]:
  row=next(r for r in rows if r['file'].endswith('/nvn/'+source.name));cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(out/(source.stem+'-android.o'));cmd[1:1]=['-ivfsoverlay',str(overlay)];run(cmd,source.stem+'-android-full-tu.log',row['directory'])
 assert pins=={str(p.relative_to(ROOT)):sha(p)for p in [*paths,exec_source]},'Source changed during verification'
 report={'schema':2,'scope':'Actual backend_execute (including mandatory list submission), queue-marker completion, monotonic publication and NVN wrappers. Controlled queue/GPU/host-mirror storage; destruction tail is explicitly a model of alias-preservation recording, not the full production destroy path or proof of an Echoes failure. Original ordinary IT_EXEC mapping PASSES with actual backend_execute. Only original QueueFenceSync behavior fails the modeled non-list pending destruction-tail contract. NDEBUG optimized+ASan/UBSan and Android objects/rqueue TUs. Finite timeout remains blocking. Earlier prepatch IT_EXEC-failure qualification was incorrect and is withdrawn.','tests':reports,'old_ordinary_it_exec_passed':prior_ordinary_passed,'old_marker_failed_modeled_destroy_tail':prior_destroy_failed,'android_full_tus':True,'source_sha256':pins};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
