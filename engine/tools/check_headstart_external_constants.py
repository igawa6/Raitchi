#!/usr/bin/env python3
"""Asset-free original/replay BRX counterexample and real cache-admission tests."""
import argparse,hashlib,json,pathlib,re,shlex,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def function(s,signature):
 start=s.index(signature);a=s.index('{',start);depth=1;i=a+1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]+'\n'
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--android',action='store_true');p.add_argument('--cache',type=pathlib.Path);a=p.parse_args();out=ROOT/'build/headstart-external-constants';out.mkdir(parents=True,exist_ok=True);source=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c';translator=ROOT/'ports/shared-android/runtime/nvn/maxwell/translate.c';test=ROOT/'tests/headstart_external_constants_test.c';s=source.read_text();t=translator.read_text()
 policy=s[s.index('// BEGIN headstart external-constant policy'):s.index('// END headstart external-constant policy')];fragment=s[s.index('#define HS_MAGIC'):s.index('// BEGIN headstart cache identity')]+re.search(r'typedef struct \{ uint64_t chash, cmax\[2\], csize\[2\]; ShaderKey vkey, fkey; \} HsProgHeader;',s).group(0)+'\n'+function(s,'static bool hs_program_size_valid(')+policy+function(s,'static const uint8_t *prog_code(')+function(s,'static uint64_t hash_words(uint64_t h, const void *p, size_t n) {')+function(s,'static uint64_t prog_content_hash(')+function(s,'static void hs_record_program_closure(const Prog *p) {')+function(s,'static void hs_record_program(const Prog *p) {')+function(s,'static void hs_record_pipe(')+function(t,'static uint32_t norm_target(')+function(t,'static bool brx_table(');(out/'headstart_external_production.h').write_text(fragment)
 replay=s[s.index('static void *headstart_thread('):s.index('static void *headstart_thread(')+10000];assert replay.index('if (!hs_program_replayable(')<replay.index('Prog *p = calloc(')<replay.index('run_translate_q(')
 live=s[s.index('static Prog *get_program('):s.index('// ================================================================== vertex formats')];assert 'Prog *p = replayable ? hs_take(ch) : NULL;' in live and 'if (replayable) p = hs_add(candidate, true, &fresh);' in live
 paths=[source,translator,test,ROOT/'tests/headstart_translate_test.c',pathlib.Path(__file__).resolve()];pins={str(x.relative_to(ROOT)):sha(x) for x in paths}
 def run(args,name,cwd=None):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=90,cwd=cwd);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={};cache_report={};translation={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',out,'-I',ROOT/'ports/shared-android/runtime',test,'-o',exe],name+'-build.log');reports[name]=json.loads(run([exe],name+'-run.log'))
  if a.cache: raise ValueError('LHS6 and earlier private caches are intentionally incompatible with LHS7')
  whole=out/(name+'-translator');run([shutil.which('clang'),'-std=c11','-D_GNU_SOURCE',*flags,'-I',translator.parent,'-I',ROOT/'native',ROOT/'tests/headstart_translate_test.c',translator,'-lm','-o',whole],name+'-translator-build.log');translation[name]=json.loads(run([whole,out],name+'-translator-run.log'))
 glslang=shutil.which('glslangValidator');val=shutil.which('spirv-val');assert glslang and val
 run([glslang,'-V','--target-env','vulkan1.3',out/'headstart-brx.vert','-o',out/'headstart-brx.spv'],'brx-glslang.log');run([val,'--target-env','vulkan1.3',out/'headstart-brx.spv'],'brx-spirv-val.log')
 android={}
 if a.android:
  rows=json.loads((ROOT/'build/shared-android/native/compile_commands.json').read_text());row=next(x for x in rows if x['file'].endswith('/runtime/nvn/vulkan/exec.c'));cmd=shlex.split(row['command']);obj=out/'exec-android.o';cmd[cmd.index('-o')+1]=str(obj);overlay=out/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(ROOT/'work/shared-android/runtime/nvn/vulkan/exec.c'),'external-contents':str(source)}]}));run([*cmd,'-ivfsoverlay',overlay],'android-production-build.log',row['directory']);android={'passed':True,'object_sha256':sha(obj)}
 assert pins=={k:sha(ROOT/k) for k in pins},'Source changed during checks'
 report={'state':'passed','host_tests':reports,'full_translator_parity':translation,'brx_spirv_validated':True,'private_cache_scan':cache_report,'android_production_compile':android,'source_sha256':pins,'scope':'Real bounded BRX resolver with omitted/restored-constant counterexample, exact constant record payloads and content hashes; actual recording/admission functions and foreground/replay source gates. No GPU compile or device game claim.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
