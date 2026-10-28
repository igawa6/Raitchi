#!/usr/bin/env python3
"""Build asset-free actual native-block/Unicorn diagnostic; no device actions."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--global-page-limit',type=int,choices=(0,2,32768),default=32768)
    parser.add_argument('--ndk',type=Path,default=Path.home()/'android-sdk/ndk/27.2.12479018')
    parser.add_argument('--unicorn-include',type=Path,default=ROOT/'build/shared-ondevice/work/build/ondevice/src/unicorn-2.1.4/src/include')
    parser.add_argument('--unicorn-library',type=Path,default=ROOT/'build/shared-android/apk-inputs/jniLibs/arm64-v8a/libunicorn.so')
    parser.add_argument('--capstone-include',type=Path,default=ROOT/'build/shared-ondevice/work/build/ondevice/src/capstone-5.0.9/src/include')
    args=parser.parse_args();out=ROOT/('build/native-blocks-probe' if args.global_page_limit==32768 else 'build/native-blocks-probe-limit'+str(args.global_page_limit));out.mkdir(parents=True,exist_ok=True)
    cc=shutil.which('clang');assert cc,'Host clang required'
    target=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin';clang=target/'aarch64-linux-android30-clang'
    def run(command,name):
        result=subprocess.run(list(map(str,command)),text=True,capture_output=True,timeout=30)
        (out/name).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(str(command[0])+' failed: '+result.stderr)
        return result.stdout
    sources=[ROOT/'tests/native_blocks_probe.c',Path(__file__).resolve(),ROOT/'ports/shared-android/runtime/native_blocks.c',ROOT/'ports/shared-android/runtime/native_blocks.S',ROOT/'ports/shared-android/runtime/native_blocks.h',ROOT/'native/runtime/aot_cpu.h',ROOT/'native/native-game-backend/block_plan.c',ROOT/'native/native-game-backend/block_plan.h']
    source_hashes={str(p.relative_to(ROOT)):sha(p) for p in sources}
    programs={
        'gpr':[f'add x{i},x{i},#{i+1}' for i in range(31) if i not in (18,19)]+['cmp x0,x0'],
        'vector':[f'eor v{i}.16b,v{i}.16b,v31.16b' for i in range(32)],
        'fp':['fdiv s0,s1,s2'],
        'stack':['sub sp,sp,#64','stp x16,x17,[sp]','stp x29,x30,[sp,#16]','str q31,[sp,#32]','ldp x16,x17,[sp,#16]','ldp x29,x30,[sp]','ldr q0,[sp,#32]','mov x0,sp'],
        'pop':['ldp x1,x2,[sp]','add sp,sp,#64'],
        'context19':['add x19,x19,#7','add x16,x19,x17','eor v19.16b,v19.16b,v18.16b','add x30,x30,#11','adds x1,x19,#1','mov x20,x19'],
        'stack19':['mov x19,sp','sub sp,sp,#32','stp x16,x17,[sp]','ldp x20,x21,[x19,#-32]','add x19,x19,#16','str x19,[sp,#16]'],
        'temp17':[f'add x{i},x{i},#{i+1}' for i in range(31) if i not in (17,18)]+['cmp x0,x0'],
        'temp30':[f'add x{i},x{i},#{i+1}' for i in range(31) if i not in (18,30)]+['cmp x0,x0'],
        'exclusive':['ldxr x0,[x2]','add x0,x0,#1','stxr w1,x0,[x2]','clrex'],
    }
    data={};masks={};header=['/* Generated from asset-free assembly by check_native_blocks.py. */']
    for name,instructions in programs.items():
        assembly=out/(name+'.S');assembly.write_text('.text\n'+ '\n'.join(instructions)+'\n')
        obj=out/(name+'.o');binary=out/(name+'.bin')
        run([clang,'-c',assembly,'-o',obj],name+'-assemble.log')
        run([target/'llvm-objcopy','--only-section=.text','-O','binary',obj,binary],name+'-extract.log')
        raw=binary.read_bytes();assert len(raw)==len(instructions)*4
        words=struct.unpack('<'+'I'*len(instructions),raw);data[name]=list(words)
        masks[name]=sum(1<<int(reg) for reg in set(re.findall(r'\b[xw]([0-9]+)\b',';'.join(instructions))))
        header.append('#define PROBE_'+name.upper()+'_MASK '+hex(masks[name])+'u')
        header.append('static const uint32_t probe_'+name+'_words[]={'+','.join(hex(w)+'u' for w in words)+'};')
    names=[name for name in programs if name!='exclusive']
    header+=['#define PROBE_PROGRAM_COUNT '+str(len(names)),'static const uint32_t *const probe_programs[]={'+','.join('probe_'+name+'_words' for name in names)+'};','static const size_t probe_counts[]={'+','.join(str(len(data[name])) for name in names)+'};','static const uint32_t probe_masks[]={'+','.join(hex(masks[name])+'u' for name in names)+'};']
    (out/'native_blocks_words.h').write_text('\n'.join(header)+'\n')
    tests={}
    for limit in (0,2):
      for variant,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        name=variant+'-limit'+str(limit);binary=out/('host-'+name)
        run([cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,'-DNATIVE_BLOCKS_HOST_TEST','-DNATIVE_BLOCKS_GLOBAL_PAGE_LIMIT='+str(limit),'-I',ROOT/'native','-I',ROOT/'ports/shared-android/runtime','-I',ROOT/'native/native-game-backend','-I',args.capstone_include,'-I',out,ROOT/'tests/native_blocks_probe.c',ROOT/'native/native-game-backend/block_plan.c',ROOT/'ports/shared-android/runtime/native_blocks.c','-pthread','-lcapstone','-Wl,--wrap=mmap','-Wl,--wrap=mprotect','-o',binary],name+'-build.log')
        stdout=run([binary],name+'-run.log');tests[name]={'passed':True,'assertions':int(re.search(r'assertions=(\d+)',stdout)[1]),'scope':'actual planner/selector + real cache emission/W^X/forced-capacity/churn/cleanup/reentry/resource failure with explicit nonexecuting host entry stub'}
    exports=out/'exports.map';exports.write_text('NATIVE_BLOCKS_PROBE_1 { global: native_blocks_probe; local: *; };\n')
    helper=out/'libnative_blocks_probe.so'
    run([clang,'-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-fPIC','-ffixed-x18','-fvisibility=hidden','-shared','-DNATIVE_BLOCKS_GLOBAL_PAGE_LIMIT='+str(args.global_page_limit),'-I',ROOT/'native','-I',ROOT/'ports/shared-android/runtime','-I',args.unicorn_include,'-I',out,ROOT/'tests/native_blocks_probe.c',ROOT/'ports/shared-android/runtime/native_blocks.c',ROOT/'ports/shared-android/runtime/native_blocks.S','-ldl','-pthread','-Wl,--wrap=mmap','-Wl,--wrap=mprotect','-Wl,--no-undefined','-Wl,--version-script='+str(exports),'-Wl,-z,max-page-size=16384','-Wl,-z,relro','-Wl,-z,now','-Wl,-z,noexecstack','-o',helper],'android-build.log')
    symbols=run([target/'llvm-nm','--dynamic','--defined-only',helper],'exports.log');assert [line.split()[-1].split('@')[0] for line in symbols.splitlines()]==['native_blocks_probe']
    assembly=run([target/'llvm-objdump','--no-show-raw-insn','-d',helper],'disassembly.txt')
    bad=[]
    for line in assembly.splitlines():
        match=re.match(r'\s*[0-9a-f]+:\s+([a-z0-9.]+)\s+(.*)',line)
        if not match:continue
        op,operands=(part.lower() for part in match.groups())
        if ((op=='msr' and re.match(r'tpidr(?:ro)?_el0\b',operands)) or (re.match(r'[xw]18(?:,|$)',operands) and op not in ('str','stur','stlr','stp','stnp')) or (op in ('ldp','ldnp') and re.match(r'[^,]+,\s*[xw]18\b',operands))):bad.append((op,operands))
    assert not bad,'Physical X18/TLS write: '+repr(bad)
    config={'schema':1,'helper_sha256':sha(helper),'unicorn_sha256':sha(args.unicorn_library),'outer_timeout_seconds':30,'global_page_limit':args.global_page_limit}
    (out/'config.json').write_text(json.dumps(config,indent=2)+'\n')
    runner=out/'run_probe.py';runner.write_text('''import ctypes,hashlib,json,os,sys
config=json.load(open(sys.argv[1]));helper,unicorn=sys.argv[2:4]
for path,key in ((helper,'helper_sha256'),(unicorn,'unicorn_sha256')):
 with open(path,'rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
 if digest!=config[key]: raise SystemExit('Hash mismatch: '+key)
lib=ctypes.CDLL(helper);probe=lib.native_blocks_probe;probe.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t];probe.restype=ctypes.c_int
output=ctypes.create_string_buffer(4096);status=probe(os.fsencode(unicorn),output,len(output));report=json.loads(output.value)
if report['global_page_limit']!=config['global_page_limit']:raise SystemExit('Global cache limit mismatch')
report.update({k:config[k] for k in ('helper_sha256','unicorn_sha256')});print(json.dumps(report),flush=True);raise SystemExit(status!=0)
''')
    assert source_hashes=={str(p.relative_to(ROOT)):sha(p) for p in sources},'Source changed during verification'
    report={'schema':1,'host_tests':tests,'android_cross_link_passed':True,'device_execution':'pending-coordinator','helper_sha256':sha(helper),'unicorn_sha256':sha(args.unicorn_library),'sources_sha256':source_hashes,'headers_sha256':{str(p):sha(p) for p in [args.unicorn_include/'unicorn/unicorn.h',args.unicorn_include/'unicorn/arm64.h']},'artifacts_sha256':{str(p.relative_to(ROOT)):sha(p) for p in [helper,out/'config.json',runner,out/'native_blocks_words.h']},'program_instructions':programs,'physical_x18_tls_writes':bad,'scope':'synthetic Cpu/guarded anonymous stacks; actual native adapter plus bundled Unicorn exclusive-monitor behavior; no KThread/title/game proof'}
    (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'host_tests':tests,'helper_sha256':sha(helper),'unicorn_sha256':sha(args.unicorn_library),'android_cross_link_passed':True},indent=2))

if __name__=='__main__':main()
