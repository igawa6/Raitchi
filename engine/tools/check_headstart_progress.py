#!/usr/bin/env python3
"""Actual headstart reader and caller gate with delayed shader/pipeline workers."""
import hashlib,json,pathlib,re,shutil,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1];SOURCE=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c';OUT=ROOT/'build/headstart-progress';TEST=ROOT/'tests/headstart_progress_test.c'
def function(s,signature):
 start=s.index(signature);a=s.index('{',start);depth=1;i=a+1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]+'\n'
def run(args,cwd=None):
 p=subprocess.run(list(map(str,args)),cwd=cwd,capture_output=True,text=True,timeout=30)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout.strip()
def main():
 OUT.mkdir(parents=True,exist_ok=True);s=SOURCE.read_text()
 fragment=s[s.index('#define HS_MAGIC'):s.index('// BEGIN headstart cache identity')]+re.search(r'typedef struct \{ uint64_t chash, cmax\[2\], csize\[2\]; ShaderKey vkey, fkey; \} HsProgHeader;',s).group(0)+'\n'+re.search(r'typedef struct \{\n    uint32_t magic, header_size, pipe_size, shader_key_size;\n    uint8_t identity\[32\];\n\} HsHeader;',s).group(0)+'\ntypedef struct { uint32_t type, size; } HsRec;\nstatic HsHeader hs_header;\n'
 fragment+=re.search(r'typedef struct \{ uint64_t stage_hash; uint32_t local\[3\], code_bytes, constant_bytes; ShaderKey key; \} HsCsHeader;',s).group(0)+'\n'+re.search(r'(?m)^#define HS_CS_TAB \d+',s).group(0)+'\n'
 # The real reader/progress loop is exercised here; compute validation/reuse
 # has its own production-helper fixture in check_compute_headstart.py.
 fragment+='static CsProg *hs_cs_replay(uint32_t size,const HsCsHeader *header,const void *bytes,bool *fresh){(void)header;return test_compute_replay(size,bytes,fresh);}\n'
 for sig in ['static bool hs_program_size_valid(','static uint64_t hash_words(uint64_t h, const void *p, size_t n) {','static uint64_t prog_content_hash(','static bool hs_stage_replayable(','static bool hs_program_replayable(','static Prog *hs_add_locked(','static Prog *hs_add(','static Prog *hs_take(','static uint64_t pipe_key_hash(const PipeKey *k) {','static Pipe *hs_pipe_put(','static void *headstart_thread(','void vk_headstart_begin(','void vk_headstart_prepare(','void vk_headstart_finish(']:fragment+=function(s,sig)
 (OUT/'headstart_progress_production.h').write_text(fragment);results={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/name;run([shutil.which('clang'),'-std=gnu11','-Wall','-Wextra','-Werror',*flags,'-I',OUT,'-I',ROOT/'ports/shared-android/runtime',TEST,'-pthread','-o',exe]);results[name]=[]
  for mode in ['full','empty','corrupt','cancel','failmodule','failpipe','compute']:
   with tempfile.TemporaryDirectory(prefix='headstart-progress-') as d:results[name].append(run([exe,mode],d))
 report={'state':'passed','results':results,'scope':'Actual production reader, ownership helpers, payload/hash validation and blocking preparation API; all present counts remain zero. Delayed worker doubles verify return after completed pipeline, empty-cache return, mutated translation-key rejection, cancellation with terminal drain, and module/pipeline failures. Not Vulkan/driver or device validation.','source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [SOURCE,TEST,pathlib.Path(__file__).resolve()]}};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
