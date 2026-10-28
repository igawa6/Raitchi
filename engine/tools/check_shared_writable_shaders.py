#!/usr/bin/env python3
"""Host-only shared Maxwell writable-mirror checks; no device or guest execution.

Synthetic LDG/STG fixtures exercise actual translation, GLSL compilation and
SPIR-V validation. Optional --capture is an ignored/private raw compute blob.
The emitted uint32 address arithmetic is executed with synthetic host inputs;
this is not GPU memory-ordering, map-capacity or gameplay validation.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import random
import re
import shutil
import shlex
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x1240000000
SIZE = 0xD0000000
MASK = (1 << 64) - 1
RUNNER = r'''
#include "shader.h"
#include <stdio.h>
#include <stdlib.h>
bool shared_title_las(void) { return true; }
int main(int argc,char**argv) {
 if(argc!=7)return 2;
 FILE*f=fopen(argv[1],"rb");if(!f)return 3;
 fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
 unsigned char*b=calloc(1,(size_t)n+256);if(!b)return 4;
 if(fread(b,1,n,f)!=(size_t)n)return 4;fclose(f);
 size_t limit=strtoull(argv[5],NULL,0);if(limit>(size_t)n)return 5;
 maxwell_gmem_base=0x1240000000ull;maxwell_gmem_size=0xd0000000ull;
 maxwell_gmem_mirror=1;maxwell_gmem_zero=1;maxwell_gfind_cache=1;
 maxwell_shader_int64=atoi(argv[3]);maxwell_gmem_readonly=atoi(argv[4]);
 ShaderKey k={0};k.glsl=1;k.vs_out_mask=3;
 ShaderInfo*s=maxwell_translate(b,limit,atoi(argv[2]),&k);
 if(!s->msl){fprintf(stderr,"translation failed: %s\n",s->error);return 6;}
 f=fopen(argv[6],"wb");if(!f)return 7;fputs(s->msl,f);fclose(f);
 printf("map=%d stores=%d readonly=%d\n",s->uses_gmem_map,s->uses_gmem_store,s->uses_gmem_readonly);
 maxwell_free(s);free(b);return 0;
}
'''


def run(command, *, expected=0):
    p = subprocess.run([str(x) for x in command], text=True, capture_output=True)
    if p.returncode != expected:
        raise RuntimeError(f"Command failed ({p.returncode}): {command}\n{p.stdout}{p.stderr}")
    return p.stdout + p.stderr


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def blob(store, size, offset, wide=True):
    memory = 0xEED0000000000000 | (int(store) << 51) | (size << 48) | (int(wide) << 45)
    memory |= ((offset & 0xFFFFFF) << 20) | (7 << 16) | (8 << 8) | 0
    return struct.pack('<4Q', 0, memory, 0xE30000000007000F, 0x50B0000000070F00)


def ensure_no_int64(path):
    data = path.read_bytes()
    if len(data) % 4:
        raise RuntimeError('Invalid SPIR-V byte size')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    if not words or words[0] != 0x07230203:
        raise RuntimeError('Invalid SPIR-V magic')
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        if not count or i + count > len(words):
            raise RuntimeError('Invalid SPIR-V instruction size')
        if op == 17 and count >= 2 and words[i + 1] == 11:
            raise RuntimeError('Unexpected Int64 capability in uint32 shader')
        if op == 21 and count >= 4 and words[i + 2] == 64:
            raise RuntimeError('Unexpected 64-bit integer type in uint32 shader')
        i += count


def address_checks(source, offset, directory, cc):
    # Execute the production translator's emitted arithmetic, not a duplicate
    # implementation of carry/borrow. Only synthetic scalar address expressions
    # are accepted by this extraction pattern and compiled as a host C helper.
    match = re.search(r'\{\s*uint al = .*?bool ok = .*?;', source, re.S)
    if not match:
        raise RuntimeError('No emitted uint32 address calculation found')
    body = match.group(0)[1:]
    if offset == 0 and ('lo = al, hi = ah;' not in body or 'lo < al' in body):
        raise RuntimeError('Zero-offset address did not use the direct low/high pair')
    body = re.sub(r'\buint\(([^()]*)\)', r'((uint32_t)(\1))', body)
    body = re.sub(r'\buint\b', 'uint32_t', body)
    c = directory / ('address' + str(offset) + '.c')
    lib = c.with_suffix('.so')
    c.write_text('#include <stdint.h>\n#include <stdbool.h>\n'
                 'int evaluate(uint32_t r8,uint32_t r9,uint32_t*out){\n' + body +
                 '\n*out=ga;return ok;}\n')
    run([cc, '-shared', '-fPIC', '-O2', c, '-o', lib])
    loaded = ctypes.CDLL(str(lib))
    f = loaded.evaluate
    f.argtypes = (ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32))
    f.restype = ctypes.c_int
    points = [0, MASK, BASE - 1, BASE, BASE + 1, BASE + SIZE - 1, BASE + SIZE,
              (BASE & ~0xFFFFFFFF) - 1, BASE & ~0xFFFFFFFF, BASE + (1 << 32)]
    wrap = (BASE & ~0xFFFFFFFF) + (1 << 32)
    # Check inclusive/exclusive endpoints and the LOW-word rollover explicitly,
    # with inputs adjusted so adding each signed immediate lands at the target.
    points += [((edge + delta) - offset) & MASK for edge in (BASE, wrap, BASE + SIZE)
               for delta in (-4, -1, 0, 1, 4)]
    rng = random.Random(0xA64)
    points += [rng.randrange(1 << 64) for _ in range(128)]
    points += [((BASE + rng.randrange(SIZE)) - offset) & MASK for _ in range(128)]
    for address in points:
        ga = ctypes.c_uint32()
        ok = f(address & 0xFFFFFFFF, address >> 32, ctypes.byref(ga))
        expected = (address + offset - BASE) & MASK
        if ok != int(expected < SIZE) or ga.value != (expected & 0xFFFFFFFF):
            raise RuntimeError('Emitted uint32 carry/borrow differs from 64-bit reference')
    return len(points)




def iadd_blob(immediate, x, wcc, negate):
    op = 0x1C00000000000000 | (int(negate) << 56) | (int(x) << 53) | (int(wcc) << 52)
    op |= ((immediate & 0xFFFFFFFF) << 20) | (7 << 16) | (8 << 8)
    store = 0xEED0000000000000 | (1 << 51) | (4 << 48) | (1 << 45) | (7 << 16) | (8 << 8)
    return struct.pack('<4Q', 0, op, store, 0xE30000000007000F)


def iadd_checks(source, immediate, x, wcc, negate, directory, cc, name):
    match = re.search(r'\{\s*(?:uint carry;\s*)?uint sum = [^;]+;(?: cc = carry;)?', source)
    if not match:raise RuntimeError('Expected emitted IADD32I operation')
    body = match.group(0)[1:]
    if wcc and ('add32(' not in body or 'cc = carry' not in body):
        raise RuntimeError('Carry-producing IADD32I stopped updating carry')
    helper = re.search(r'uint add32\([^\n]+', source).group(0)
    helper = helper.replace('out uint co', 'uint *co').replace('co =', '*co =')
    helper = re.sub(r'uaddCarry\(([^,]+), ([^,]+), ([^)]+)\)', r'uaddCarry(\1, \2, &\3)', helper)
    body = body.replace(', carry)', ', &carry)')
    text = helper + '\nuint evaluate(uint r8,uint cc,uint*out){\n' + body + '\n*out=cc;return sum;}\n'
    text = re.sub(r'\buint\b', 'uint32_t', text)
    c = directory / (name + '.c');lib = c.with_suffix('.so')
    c.write_text('#include <stdint.h>\nstatic uint32_t uaddCarry(uint32_t a,uint32_t b,uint32_t*c)'
                 '{uint64_t s=(uint64_t)a+b;*c=(uint32_t)(s>>32);return (uint32_t)s;}\n'+text)
    run([cc,'-shared','-fPIC','-O2',c,'-o',lib])
    h=ctypes.CDLL(str(lib));f=h.evaluate
    f.argtypes=(ctypes.c_uint32,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32));f.restype=ctypes.c_uint32
    rng=random.Random(0x1ADD)
    values=[0,1,2,0x7FFFFFFF,0x80000000,0xFFFFFFFE,0xFFFFFFFF]+[rng.randrange(1<<32) for _ in range(32)]
    checks=0
    for a in values:
        for ci in (0,1):
            before=(-a if negate else a)&0xFFFFFFFF
            expected=before+immediate+(ci if x else 0)
            co=ctypes.c_uint32()
            got=f(a,ci,ctypes.byref(co))
            if got!=expected&0xFFFFFFFF or co.value!=((expected>>32)&1 if wcc else ci):
                raise RuntimeError('Emitted IADD32I differs from modular/carry reference')
            checks+=1
    return checks

def map_checks(source, directory, cc):
    index = re.search(r'uint gmap_index\([^\n]+', source)
    lookup = re.search(r'uint gfind\([^\n]+', source)
    globals_ = re.search(r'uint gc_lo[^\n]+', source)
    if not all((index, lookup, globals_)):
        raise RuntimeError('Expected actual emitted cached map helpers')
    code = '\n'.join(m.group(0) for m in (index, globals_, lookup))
    code = code.replace('uint(gfast_.w.length())', 'gfast_count')
    code = re.sub(r'uvec4\(([^()]*)\)', r'((U4){\1})', code)
    code = re.sub(r'\buvec4\b', 'U4', code)
    code = re.sub(r'\buint\b', 'uint32_t', code)
    c = directory / 'map_helper.c';lib = c.with_suffix('.so')
    c.write_text('#include <stdint.h>\ntypedef struct {uint32_t x,y,z,w;} U4;\n'
                 'static U4 gmap[33];static uint32_t gfast_count;\n'
                 'static uint32_t min(uint32_t a,uint32_t b){return a<b?a:b;}\n' + code +
                 '\nvoid setup(uint32_t a,uint32_t n,uint32_t w,uint32_t cap){'
                 'for(int i=0;i<33;i++)gmap[i]=(U4){0};gmap[0]=(U4){a,n,w,0};'
                 'gmap[32].x=1;gfast_count=cap;gc_lo=gc_sz=gc_w=0;}\n')
    run([cc, '-shared', '-fPIC', '-O2', c, '-o', lib])
    h = ctypes.CDLL(str(lib));h.setup.argtypes = (ctypes.c_uint32,)*4
    f = h.gfind;f.argtypes = (ctypes.c_uint32,)*2;f.restype = ctypes.c_uint32
    h.setup(100,16,10,32)
    cases = [(100,4,10),(112,4,13),(112,8,0xFFFFFFFF),(108,8,12),(100,16,10),
             (104,16,0xFFFFFFFF),(96,4,0xFFFFFFFF),(100,0,0xFFFFFFFF)]
    for a,n,w in cases:
        if f(a,n)!=w:raise RuntimeError('Map span/cached lookup crossed a bound range')
    for cap,n,expected in ((13,16,0xFFFFFFFF),(12,8,10),(12,16,0xFFFFFFFF),(10,4,0xFFFFFFFF)):
        h.setup(100,16,10,cap)
        if f(100,n)!=expected:raise RuntimeError('Map lookup crossed backing storage limit')
    h.setup(100,16,0xFFFFFFFF,32)
    if f(100,4)!=0xFFFFFFFF:raise RuntimeError('Map word offset overflow accepted')
    return len(cases)+5


def exec_checks(directory, cc):
    path = ROOT / 'ports/shared-android/runtime/nvn/vulkan/exec.c'
    text = path.read_text()
    match = re.search(r'static bool mirror_guest_range\([^}]+\}', text)
    if not match:raise RuntimeError('Production mirror range validator missing')
    helper = match.group(0).replace('static bool ', 'bool ',1)
    c = directory / 'mirror_range.c';lib = c.with_suffix('.so')
    c.write_text('#include <stdint.h>\n#include <stdbool.h>\n'
                 '#define HEAP_BASE 0x1240000000ull\n#define HEAP_MAX 0xd0000000ull\n' + helper)
    run([cc,'-shared','-fPIC','-O2',c,'-o',lib])
    h=ctypes.CDLL(str(lib));f=h.mirror_guest_range
    f.argtypes=(ctypes.c_uint64,ctypes.c_uint64,ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint64))
    f.restype=ctypes.c_bool
    cases=[(BASE,4,True),(BASE+1,1,True),(BASE+SIZE-1,1,True),(BASE,SIZE,True),
           (BASE,0,False),(BASE-1,1,False),(BASE+SIZE,1,False),(BASE+SIZE-1,2,False),
           (BASE,MASK,False),(BASE+4096,(-BASE-4096)&MASK,False),(MASK-3,16,False)]
    for addr,size,valid in cases:
        lo,hi=ctypes.c_uint64(),ctypes.c_uint64()
        if f(addr,size,ctypes.byref(lo),ctypes.byref(hi))!=valid:
            raise RuntimeError('Mirror guest range accepted wrap/outside input or rejected valid input')
        if valid and (lo.value!=addr&~3 or hi.value!=(addr+size+3)&~3):
            raise RuntimeError('Mirror word rounding changed the validated range')
    return len(cases)


def compile_android_exec(directory):
    commands=ROOT/'build/shared-android/native/compile_commands.json'
    if not commands.is_file():return 'not run: shared compile_commands.json absent'
    entries=json.loads(commands.read_text())
    entry=next(e for e in entries if e['file'].endswith('/nvn/vulkan/exec.c'))
    tree=directory/'runtime'
    for src in (ROOT/'sources/las-android/runtime',ROOT/'ports/shared-android/runtime'):
        for header in src.rglob('*.h'):
            target=tree/header.relative_to(src);target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(header,target)
    target=tree/'nvn/vulkan/exec.c';target.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(ROOT/'ports/shared-android/runtime/nvn/vulkan/exec.c',target)
    cmd=shlex.split(entry['command']);cmd[cmd.index('-c')+1]=str(target)
    cmd[cmd.index('-o')+1]=str(directory/'exec.android.o')
    cmd[1:1]=['-I'+str(tree),'-I'+str(ROOT/'native')]
    run(cmd)
    return 'full Android ARM64/API30 translation unit passed'

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--skip-android-exec', action='store_true', help='Translator-only checks; omit unchanged exec.c NDK compilation')
    ap.add_argument('--capture', type=Path, help='Private raw LAS storing-compute dump (not copied into tracked files)')
    ap.add_argument('--output', type=Path, default=ROOT / 'build/shared-writable-shaders')
    args = ap.parse_args()
    out = args.output.resolve();out.mkdir(parents=True, exist_ok=True)
    cc = shutil.which('clang')
    glslang = shutil.which('glslangValidator')
    validator = shutil.which('spirv-val')
    if not all((cc, glslang, validator)):
        raise SystemExit('clang, glslangValidator and spirv-val are required; nothing is downloaded.')
    source = ROOT / 'ports/shared-android/runtime/nvn/maxwell/translate.c'
    header = source.with_name('shader.h')
    initial = {'translate.c': sha(source), 'shader.h': sha(header)}
    c = out / 'translate_runner.c';c.write_text(RUNNER)
    runner = out / 'translate_runner'
    run([cc, '-O1', '-g', '-I' + str(header.parent), '-I' + str(ROOT / 'native'), c, source, '-lm', '-o', runner])
    results = []
    def check(name, binary, int64, readonly, limit, expect_store):
        path = out / (name + '.glsl');spv = path.with_suffix('.spv')
        translated = run([runner, binary, 5, int(int64), int(readonly), limit, path])
        if expect_store and 'stores=1' not in translated:
            raise RuntimeError('Storing fixture did not retain a global store')
        if not readonly and 'map=1' not in translated:
            raise RuntimeError('Writable fixture did not use GMap/GFast')
        run([glslang, '-V', '--target-env', 'vulkan1.3', '-S', 'comp', path, '-o', spv])
        run([validator, '--target-env', 'vulkan1.3', spv])
        if not int64:
            ensure_no_int64(spv)
        results.append({'case': name, 'int64': bool(int64), 'readonly': bool(readonly),
                        'global_store': expect_store, 'spirv_bytes': spv.stat().st_size})
        return path.read_text()
    for size in range(7):
        for store in (False, True):
            name = ('stg' if store else 'ldg') + str(size)
            binary = out / (name + '.synthetic');binary.write_bytes(blob(store, size, -4))
            check(name + '-writable32', binary, False, False, 32, store)
            check(name + '-writable64', binary, True, False, 32, store)
            if not store:
                check(name + '-readonly32', binary, False, True, 32, False)
    iadds = 0
    for immediate in (0,1,0x7FFFFFFF,0x80000000,0xFFFFFFFF):
        for x in (False,True):
            for negate in (False,True):
                name=f'iadd32i-{immediate:x}-x{int(x)}-neg{int(negate)}'
                binary=out/(name+'.synthetic');binary.write_bytes(iadd_blob(immediate,x,False,negate))
                src=check(name,binary,False,False,32,True)
                iadds+=iadd_checks(src,immediate,x,False,negate,out,cc,name)
    for x in (False,True):
        for negate in (False,True):
            name=f'iadd32i-carry-x{int(x)}-neg{int(negate)}'
            binary=out/(name+'.synthetic');binary.write_bytes(iadd_blob(0xFFFFFFFF,x,True,negate))
            src=check(name,binary,False,False,32,True)
            iadds+=iadd_checks(src,0xFFFFFFFF,x,True,negate,out,cc,name)
    arithmetic = 0
    spans = map_checks((out/'stg6-writable32.glsl').read_text(), out, cc)
    ranges = exec_checks(out, cc)
    for offset in (-0x800000, -1, 0, 1, 0x7FFFFF):
        binary = out / ('address' + str(offset) + '.synthetic');binary.write_bytes(blob(False, 4, offset))
        src = check('address' + str(offset), binary, False, False, 32, False)
        arithmetic += address_checks(src, offset, out, cc)
    if args.capture:
        capture = args.capture.resolve()
        if not capture.is_file() or capture.stat().st_size < 65536:
            raise RuntimeError('Captured compute must provide at least runtime cmax=65536 bytes')
        check('captured-las-compute-writable32', capture, False, False, 65536, True)
        check('captured-las-compute-writable64', capture, True, False, 65536, True)
    if initial != {'translate.c': sha(source), 'shader.h': sha(header)}:
        raise RuntimeError('Translator changed during verification; rerun once frozen')
    android = 'not repeated: translator-only check requested' if args.skip_android_exec else compile_android_exec(out)
    report = {'source_sha256': initial, 'cases': results, 'arithmetic_comparisons': arithmetic,
              'map_span_cases':spans,'guest_range_cases':ranges,'android_exec':android,'iadd32i_comparisons':iadds,
              'scope': 'host translation/SPIR-V and synthetic arithmetic; no GPU execution, map/readback ordering or gameplay proof'}
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(results)} GLSL/SPIR-V cases passed; {arithmetic} actual emitted-address comparisons, {iadds} IADD32I comparisons, {spans} map spans and {ranges} guest ranges passed; {android}.')


if __name__ == '__main__':
    main()
