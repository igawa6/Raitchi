#!/usr/bin/env python3
"""Actual renderer C1 staging/cache integration with a guarded source allocation."""
import hashlib,json,pathlib,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
 source=ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c';helper=ROOT/'ports/shared-android/runtime/nvn/constant_buffer.c';test=ROOT/'tests/compiler_constants_staging_test.c';s=source.read_text();out=ROOT/'build/compiler-constants-staging';out.mkdir(parents=True,exist_ok=True)
 start=s.index('static VkDescriptorBufferInfo compiler_constants_span(');at=s.index('{',start)+1;depth=1
 while depth:depth+=(s[at]=='{')-(s[at]=='}');at+=1
 (out/'compiler_constants_production.h').write_text(s[start:at]+'\n')
 assert 'return compiler_constants_span(ss, si->cbuf_size[1]);' in s
 reset=s[s.index('void vk_exec_reset('):];assert 'gcopy_gen++;' in reset
 paths=[source,helper,helper.with_suffix('.h'),test,pathlib.Path(__file__).resolve()];pins={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths};reports={}
 for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  binary=out/mode
  for step,args in [('compile',['clang','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',str(out),'-I',str(helper.parent),str(test),str(helper),'-o',str(binary)]),('run',[str(binary)])]:
   r=subprocess.run(args,capture_output=True,text=True,timeout=60);(out/f'{mode}-{step}.log').write_text(r.stdout+r.stderr)
   if r.returncode:raise RuntimeError(r.stdout+r.stderr)
   if step=='run':reports[mode]=json.loads(r.stdout)
 assert pins=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
 report={'state':'passed','host':reports,'source_sha256':pins,'scope':'Actual compiler_constants_span with minimal VkSpan/transient/ubo stubs plus real constant_buffer implementation. Guard-page20Bsource vs256/65536binding, empty-source no-read, cache no-allocation/no-read, address/source/staging keys, seq/gcopygen invalidation, device cap and destination canaries. No Vulkan/device/full build.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
