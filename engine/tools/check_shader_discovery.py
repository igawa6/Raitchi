#!/usr/bin/env python3
"""Test the actual portable shader discovery wire codec/store and production SHA-256."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
HEADER=ROOT/'ports/shared-android/runtime/nvn/shader_discovery.h'
TEST=ROOT/'tests/shader_discovery_test.c'
HASH=ROOT/'ports/shared-android/runtime/shared_hash.cpp'
OUT=ROOT/'build/shader-discovery-codec'

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def run(args,cwd=None):
    p=subprocess.run(list(map(str,args)),cwd=cwd,capture_output=True,text=True,timeout=90)
    if p.returncode: raise RuntimeError(' '.join(map(str,args))+'\n'+p.stdout+p.stderr)
    return p.stdout.strip()
def inspect_wire(path):
    b=Path(path).read_bytes()
    assert b[:8]==b'NVNDISC\0'
    version,header_bytes,count,reserved=struct.unpack_from('<IIII',b,8)
    assert version==1 and header_bytes==88 and reserved==0
    assert hashlib.sha256(b[:56]).digest()==b[56:88]
    at=88; records=[]
    for _ in range(count):
        n,=struct.unpack_from('<I',b,at);at+=4
        payload=b[at:at+n];at+=n
        digest=b[at:at+32];at+=32
        assert len(payload)==n and hashlib.sha256(payload).digest()==digest
        records.append(payload)
    assert at==len(b)
    # The exact shader spans and their FNV code identities must match the
    # independent Python oracle; constants are included in each declared span.
    def fnv64(data):
        h=0xcbf29ce484222325
        for value in data: h=((h^value)*0x100000001b3)&0xffffffffffffffff
        return h
    for payload in records:
        kind=payload[0];vs_hash,fs_hash=struct.unpack_from('<QQ',payload,1)
        code0,const0,code1,const1=struct.unpack_from('<IIII',payload,465)
        at=481;stage0=payload[at:at+code0+const0];at+=code0+const0
        assert len(stage0)==code0+const0 and fnv64(stage0)==vs_hash
        if kind==ord('G') and fs_hash:
            stage1=payload[at:at+code1+const1]
            assert len(stage1)==code1+const1 and fnv64(stage1)==fs_hash
        elif kind==ord('C'):
            assert not fs_hash and not code1 and not const1
        for seed,n in (((0x31,136),(0x72,132)) if kind==ord('G') else ((0xc3,36),)):
            raw=bytes((seed+i*29+(i>>2))&255 for i in range(n))
            assert raw in payload
    return {'bytes':len(b),'header_sha256_verified':True,'record_sha256_verified':True,'records':count,
            'exact_shader_spans_verified':True,'header_game_id':b[24:56].hex()}

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    cc=shutil.which(os.environ.get('CC','clang'));cxx=shutil.which(os.environ.get('CXX','clang++'))
    if not cc or not cxx: raise SystemExit('clang and clang++ required')
    include=[ROOT/'ports/shared-android/runtime/nvn',ROOT/'work/shared-android/runtime/nvn',ROOT/'ports/shared-android/runtime']
    results={}
    for mode,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'])]:
        cobj=OUT/(mode+'-test.o');hobj=OUT/(mode+'-hash.o');exe=OUT/(mode+'-test')
        run([cc,'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,*sum((['-I',p] for p in include),[]),TEST,'-c','-o',cobj])
        run([cxx,'-std=c++17',*flags,'-I',ROOT/'sources/las-android/runtime','-I',ROOT/'ports/shared-android/runtime','-I',ROOT/'native','-c',HASH,'-o',hobj])
        run([cxx,*flags,cobj,hobj,'-o',exe])
        with tempfile.TemporaryDirectory(prefix='shader-discovery-') as d:
            result=json.loads(run([exe,d]));wire=inspect_wire(Path(d)/'discovery.bin')
        results[mode]={'host':result,'independent_wire_check':wire}
    paths=[HEADER,TEST,HASH,Path(__file__).resolve()]
    report={'state':'passed','results':results,'source_sha256':{str(p.relative_to(ROOT)):sha(p) for p in paths},
            'scope':'Actual header-only LE codec/store with actual shared SHA256 wrapper; synthetic graphics/compute spans only. SHA known vector, game/schema rejection, renderer-independent header, duplicate/roundtrip, record tamper, torn-tail recovery, limits, invalid enums/counts/overflow, and failed atomic replacement. No game shaders, GPU, device, or renderer execution.'}
    OUT.mkdir(parents=True,exist_ok=True);(OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__': main()
