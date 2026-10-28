#!/usr/bin/env python3
"""Direct package provider checks; optional private owner-package differential reads."""
import argparse,ctypes,hashlib,json,os,pathlib,shutil,struct,subprocess,time
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p):
 with pathlib.Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=pathlib.Path,default=pathlib.Path.home()/'android-sdk/ndk/27.2.12479018');p.add_argument('--actual-import-receipt',type=pathlib.Path);p.add_argument('--inventory',type=pathlib.Path);a=p.parse_args();out=ROOT/'build/nxpkg';out.mkdir(parents=True,exist_ok=True)
 source=ROOT/'ports/shared-android/runtime/nxpkg.c';test=ROOT/'tests/nxpkg_test.c';paths=[source,source.with_suffix('.h'),test,pathlib.Path(__file__).resolve(),ROOT/'native/runtime/LICENSE-EoWRecomp'];pins={str(x.relative_to(ROOT)):sha(x) for x in paths}
 def run(args,name):
  r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=120);(out/name).write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(name+': '+r.stdout+r.stderr)
  return r.stdout
 reports={}
 for name,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
  exe=out/name;run([shutil.which('clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,test,'-pthread','-o',exe],name+'-build.log');reports[name]=json.loads(run([exe],name+'-run.log'))
 target=out/'nxpkg-test-android';run([a.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-ffixed-x18',test,'-pthread','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',target],'android-link.log')
 actual={}
 if a.actual_import_receipt:
  if not a.inventory:p.error('--inventory is required for private package verification')
  old=json.loads(a.actual_import_receipt.read_text());inv=json.loads(a.inventory.read_text());keys=next(pathlib.Path(x['path']) for x in inv['key_file_presence_only'] if pathlib.Path(x['path']).is_file());package=pathlib.Path(old['source']);reference=pathlib.Path(old['destination']);cache=out/'actual-exefs';cache.mkdir(exist_ok=True)
  libpath=out/'libnxpkg.so';run([shutil.which('clang'),'-D_GNU_SOURCE','-std=c11','-O2','-shared','-fPIC',source,'-pthread','-o',libpath],'actual-library-build.log');l=ctypes.CDLL(str(libpath))
  ptr=ctypes.c_void_p;u64=ctypes.c_uint64;cstr=ctypes.c_char_p;sz=ctypes.c_size_t
  for name,args,ret in [('nx_package_open_fd',[ctypes.c_int,cstr,ptr,sz],ptr),('nx_package_extract_exefs',[ptr,cstr,ptr,sz],ctypes.c_bool),('nx_package_open_romfs',[ptr,ptr,sz],ptr),('nx_romfs_clone',[ptr,ptr,sz],ptr),('nx_package_title_id',[ptr],u64),('nx_romfs_size',[ptr],u64),('nx_romfs_pread',[ptr,ptr,u64,u64],ctypes.c_ssize_t),('nx_package_close',[ptr],None),('nx_romfs_close',[ptr],None)]:
   f=getattr(l,name);f.argtypes=args;f.restype=ret
  why=ctypes.create_string_buffer(512);fd=os.open(package,os.O_RDONLY);start=time.monotonic();pkg=l.nx_package_open_fd(fd,os.fsencode(keys),why,len(why));os.close(fd)
  if not pkg:raise RuntimeError(why.value.decode())
  reader=None
  try:
   assert f'{l.nx_package_title_id(pkg):016x}'==old['program_id']
   assert l.nx_package_extract_exefs(pkg,os.fsencode(cache),why,len(why)),why.value.decode()
   exefs={x.name:sha(x) for x in cache.iterdir() if x.is_file()};assert set(exefs)==set(old['exefs'])
   for name,h in exefs.items():assert h==old['exefs'][name]['sha256'] and h==sha(reference/'exefs'/name)
   reader=l.nx_package_open_romfs(pkg,why,len(why));assert reader,why.value.decode();l.nx_package_close(pkg);pkg=None
   clone=l.nx_romfs_clone(reader,why,len(why));assert clone,why.value.decode();l.nx_romfs_close(reader);reader=clone
   def read(off,size):
    b=ctypes.create_string_buffer(size);assert l.nx_romfs_pread(reader,b,size,off)==size;return b.raw
   header=struct.unpack('<10Q',read(0,80));assert header[0]==80;dirs=read(header[3],header[4]);files=read(header[7],header[8]);file_data=header[9]
   def directory(off,depth=0):
    assert depth<128 and off+24<=len(dirs)
    parent,_,_,_,_,n=struct.unpack_from('<6I',dirs,off);assert off+24+n<=len(dirs);name=dirs[off+24:off+24+n].decode()
    return name if off==0 or parent==0xffffffff else '/'.join(x for x in (directory(parent,depth+1),name) if x)
   index={};off=0
   while off<len(files):
    parent,_,data,size,_,n=struct.unpack_from('<IIQQII',files,off);assert off+32+n<=len(files);name=files[off+32:off+32+n].decode();index['/'.join(x for x in (directory(parent),name) if x)]=(file_data+data,size);off=(off+32+n+3)&~3
   samples=[]
   names=list(old['bounded_readback_sample_files'])
   if 'region_common/movie/rd000_Title.webm' in index:names.append('region_common/movie/rd000_Title.webm')
   for name in names:
    base,size=index[name];ref=reference/'romfs'/name;assert ref.stat().st_size==size
    with ref.open('rb') as f:
     for at in sorted({0,size//2,max(0,size-257)}):
      count=min(257,size-at);f.seek(at);expected=f.read(count);got=read(base+at,count);assert got==expected;samples.append({'file':name,'offset':at,'bytes':count,'sha256':hashlib.sha256(got).hexdigest()})
   actual={'program_id':old['program_id'],'package_bytes':package.stat().st_size,'romfs_virtual_bytes':l.nx_romfs_size(reader),'romfs_index_files':len(index),'exefs_sha256':exefs,'bounded_samples':samples,'package_and_caller_fd_closed_before_reads':True,'original_reader_closed_before_clone_reads':True,'romfs_tree_written':False,'elapsed_seconds':round(time.monotonic()-start,3),'limitations':['No NCA signature or hash-tree integrity verification','Bounded resource read samples; not all resource contents hashed','No device or game execution']}
   (ROOT/'evidence/direct-nsp-provider.json').write_text(json.dumps(actual,indent=2)+'\n')
  finally:
   if pkg:l.nx_package_close(pkg)
   if reader:l.nx_romfs_close(reader)
 assert pins=={k:sha(ROOT/k) for k in pins},'Source changed during checks'
 report={'state':'passed','host_tests':reports,'android_cross_link_passed':True,'android_test_sha256':sha(target),'source_sha256':pins,'actual_package':actual,'scope':'Synthetic crypto/package/header/table bounds and8000 concurrent random reads per variant; optional original-package bounded differential reads. No device/game qualification.'};(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
