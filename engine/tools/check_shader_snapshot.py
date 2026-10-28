#!/usr/bin/env python3
"""Actual foreground shader admission with bounded immutable stage snapshots."""
import hashlib,json,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1];RT=ROOT/'ports/shared-android/runtime';OUT=ROOT/'build/shader-snapshot'
def function(s,name):
 start=s.index(name);at=s.index('{',start)+1;depth=1
 while depth:depth+=(s[at]=='{')-(s[at]=='}');at+=1
 return s[start:at]+'\n'
def main():
 OUT.mkdir(parents=True,exist_ok=True);source=RT/'nvn/vulkan/exec.c';s=source.read_text();prep=ROOT/'work/shared-android/runtime/nvn/shader_prep.c';policy=s[s.index('// BEGIN headstart external-constant policy'):s.index('// END headstart external-constant policy')];start=s.index('static pthread_once_t hs_snapshot_once');end=s.index('static void hs_record_program_closure(const Prog *p) {',start);fragment=policy+function(prep.read_text(),'uint64_t shader_code_hash(')+function(s,'static const uint8_t *prog_code(')+function(s,'static uint64_t hash_words(uint64_t h, const void *p, size_t n) {')+function(s,'static uint64_t prog_content_hash(')+s[start:end];(OUT/'shader_snapshot_production.h').write_text(fragment)
 start=s.index('    uint64_t cmax[2], csize[2] = {0};');end=s.index('    // Content identity includes',start);(OUT/'shader_snapshot_admission.inc').write_text(s[start:end]);start=s.index('    for (int i = 0; i < 2; i++) {\n        p->code[i] =',end);end=s.index('    bool fresh = true;',start);(OUT/'shader_snapshot_assignment.inc').write_text(s[start:end])
 assert 'maxwell_compile_sized(prog_code(p, 0)' in s and 'maxwell_compile_sized(prog_code(p, 1)' in s
 assert 'hs_write(HS_PROG, &h, sizeof h, prog_code(p, 0)' in s
 (OUT/'shader_snapshot_fingerprint.inc').write_text(function(s,'static void prog_fp_snapshot_init('))
 paths=[source,prep,RT/'nvn/shader_snapshot.c',RT/'nvn/shader_snapshot.h',ROOT/'tests/shader_snapshot_test.c',pathlib.Path(__file__)];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths};results={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=OUT/mode;cmd=[shutil.which('clang'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,'-I',RT,'-I',OUT,ROOT/'tests/shader_snapshot_test.c',RT/'nvn/shader_snapshot.c','-pthread','-o',exe]
  for suffix,args in [('build',cmd),('run',[exe])]:
   p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60);(OUT/f'{mode}-{suffix}.log').write_text(p.stdout+p.stderr)
   if p.returncode:raise RuntimeError(p.stdout+p.stderr)
   if suffix=='run':results[mode]=p.stdout.strip()
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}
 report={'state':'passed','results':results,'source_sha256':pins,'scope':'Actual get_program foreground snapshot admission/assignment and exact-span hash with controlled readable-copy proof; outside-heap RO/guard boundary, changed C1 identity, original-address preservation, unknown-BRX/copy failure fallback. Real store deduplicates10000variants/8000concurrentcaptures by complete content, retains bounds/generation-independent bytes and rejects failed/oversize/overbudget copies. Kernel map proof validated separately; no device/prewarm speed claim.'};(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
