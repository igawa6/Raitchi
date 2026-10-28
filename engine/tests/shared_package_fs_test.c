/* Actual FS base-image bodies; real folder layout/file I/O and controlled package reader. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <setjmp.h>
#include "romfs_build.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static const char *g_romfs_dir;
static unsigned layout_calls,layout_frees;
bool actual_romfs_layout(const char *,RomfsLayout *,char *,size_t);
void actual_romfs_layout_free(RomfsLayout *);
bool romfs_layout(const char *p,RomfsLayout *l,char*w,size_t n){layout_calls++;return actual_romfs_layout(p,l,w,n);}
void romfs_layout_free(RomfsLayout *l){layout_frees++;actual_romfs_layout_free(l);}
typedef struct NxRomfs{uint8_t data[128];uint64_t size;}NxRomfs;
static unsigned opens,closes,reads,commits,clones;
static uint64_t committed_addr,committed_size,expected_off,expected_size;
static bool bad_package,bad_clone;
static int fault_count,failure_errno;
static NxRomfs *shared_package_open_romfs(char *why,size_t n){opens++;if(bad_package){snprintf(why,n,"controlled invalid package");return NULL;}NxRomfs*r=calloc(1,sizeof*r);CHECK(r);r->size=sizeof r->data;for(unsigned i=0;i<r->size;i++)r->data[i]=(uint8_t)(i^0x5a);return r;}
static uint64_t nx_romfs_size(NxRomfs*r){return r->size;}
static void nx_romfs_close(NxRomfs*r){CHECK(r);closes++;free(r);}
static NxRomfs *nx_romfs_clone(const NxRomfs*r,char*why,size_t n){if(bad_clone){snprintf(why,n,"controlled clone failure");return NULL;}NxRomfs*copy=malloc(sizeof*copy);CHECK(copy);memcpy(copy,r,sizeof*copy);clones++;return copy;}
static void plat_commit(uint64_t addr,uint64_t size){commits++;committed_addr=addr;committed_size=size;}
static ssize_t nx_romfs_pread(NxRomfs*r,void *dst,uint64_t n,uint64_t off){
 reads++;CHECK(committed_addr==(uintptr_t)dst&&committed_size==n);CHECK(off==expected_off&&n==expected_size);
 if(fault_count){if(fault_count>0)fault_count--;if(failure_errno)errno=failure_errno;return -1;}
 if(off>=r->size)return 0;if(n>r->size-off)n=r->size-off;memcpy(dst,r->data+off,(size_t)n);return (ssize_t)n;
}
static jmp_buf fatal_env;
static bool expect_fatal;
static char fatal_message[512];
static void fatal(const char *fmt,...) __attribute__((noreturn));
static void fatal(const char *fmt,...){va_list args;va_start(args,fmt);vsnprintf(fatal_message,sizeof fatal_message,fmt,args);va_end(args);if(expect_fatal)longjmp(fatal_env,1);fprintf(stderr,"%s\n",fatal_message);exit(1);}
#define FATAL(...) fatal(__VA_ARGS__)
static void log_message(const char *fmt,...){(void)fmt;}
#define LOG(...) log_message(__VA_ARGS__)
#include "shared_package_fs_fragments.h"
typedef struct{uint64_t off,size;char*path;uint32_t meta;}RomFile;
static RomFile *rom_files;
static int rom_nfiles;
static pthread_once_t romfs_once=PTHREAD_ONCE_INIT;
static void initialize_romfs(void){base_open();}
#include "shared_package_file_fragment.h"
#define REJECT(code) do{expect_fatal=true;if(!setjmp(fatal_env)){code;CHECK(0);}expect_fatal=false;}while(0)
static void select_package(const char *uri,const char *path){if(uri)CHECK(!setenv("LARECOMP_PACKAGE_URI",uri,1));else CHECK(!unsetenv("LARECOMP_PACKAGE_URI"));if(path)CHECK(!setenv("LARECOMP_PACKAGE_PATH",path,1));else CHECK(!unsetenv("LARECOMP_PACKAGE_PATH"));}
static void package_reads(void){
 uint8_t dst[32];select_package("content://fixture/game.nsp",NULL);unsigned old_layout=layout_calls,old_opens=opens,old_closes=closes;
 base_open();CHECK(g_base_open&&g_base_package&&g_base_fd<0&&opens==old_opens+1&&layout_calls==old_layout&&base_size()==128);base_open();CHECK(opens==old_opens+1);
 expected_off=7;expected_size=sizeof dst;unsigned before=reads;CHECK(base_pread(dst,sizeof dst,7)==sizeof dst);CHECK(reads==before+1);for(unsigned i=0;i<sizeof dst;i++)CHECK(dst[i]==((i+7)^0x5a));
 expected_off=124;expected_size=sizeof dst;CHECK(base_pread(dst,sizeof dst,124)==4);expected_off=128;CHECK(base_pread(dst,sizeof dst,128)==0);
 base_close();CHECK(!g_base_open&&!g_base_package&&g_base_fd<0&&closes==old_closes+1);base_close();CHECK(closes==old_closes+1);
 // Empty URI plus local path is still a selected package, with no folder probe.
 select_package("","/controlled/game.nsp");base_open();CHECK(opens==old_opens+2&&layout_calls==old_layout);base_close();CHECK(closes==old_closes+2);
}
static void read_errors(void){
 uint8_t dst[32];select_package(NULL,"/controlled/game.nsp");base_open();expected_off=19;expected_size=sizeof dst;
 unsigned before=reads,committed=commits;fault_count=3;failure_errno=EFAULT;CHECK(base_pread(dst,sizeof dst,19)==sizeof dst);CHECK(reads-before==4&&commits-committed==4);for(unsigned i=0;i<sizeof dst;i++)CHECK(dst[i]==((i+19)^0x5a));
 before=reads;fault_count=-1;failure_errno=EFAULT;REJECT(base_pread(dst,sizeof dst,19));CHECK(reads-before==17&&strstr(fatal_message,"failed to read"));
 before=reads;fault_count=1;failure_errno=EIO;REJECT(base_pread(dst,sizeof dst,19));CHECK(reads-before==1);
 // A corrupt decoder need not set errno. Stale EFAULT must not cause retries.
 before=reads;fault_count=1;failure_errno=0;errno=EFAULT;REJECT(base_pread(dst,sizeof dst,19));CHECK(reads-before==1);fault_count=0;base_close();
}
static void invalid_package(void){
 unsigned before=layout_calls,old_opens=opens,old_closes=closes;select_package("content://bad/game.nsp","/also/bad.nsp");bad_package=true;REJECT(base_open());CHECK(!g_base_open&&!g_base_package&&g_base_fd<0&&layout_calls==before&&opens==old_opens+1&&closes==old_closes);CHECK(strstr(fatal_message,"selected game package"));bad_package=false;base_close();
}
static void write_file(const char *p,const char *data){int fd=open(p,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);size_t size=strlen(data);CHECK(write(fd,data,size)==(ssize_t)size);CHECK(!close(fd));}
static void folder_and_image(const char *folder,const char *raw_path){
 uint8_t dst[32];select_package("","");g_romfs_dir=folder;unsigned old_opens=opens;
 base_open();CHECK(g_base_open&&!g_base_package&&g_base_fd<0&&g_base.nfiles==2&&opens==old_opens);uint64_t size=base_size();CHECK(size>=ROMFS_DATA_START);CHECK(base_pread(dst,8,0)==8);uint64_t header;memcpy(&header,dst,8);CHECK(header==0x50);
 int entry=-1;for(int i=0;i<g_base.nfiles;i++)if(strstr(g_base.files[i].path,"/alpha"))entry=i;CHECK(entry>=0);CHECK(base_pread(dst,4,g_base.files[entry].start)==4&&!memcmp(dst,"ABCD",4));int saved_fd=-1;for(int i=0;i<BASE_FDS;i++)if(g_base_fds[i].fd>0)saved_fd=g_base_fds[i].fd;CHECK(saved_fd>=0);
 base_close();CHECK(fcntl(saved_fd,F_GETFD)==-1&&errno==EBADF);CHECK(!g_base.tables&&!g_base.files&&!g_base_open);
 // Keep the legacy raw-image route when no package or extracted tree exists.
 g_romfs_dir=raw_path;base_open();CHECK(g_base_open&&!g_base_package&&g_base_fd>=0&&base_size()==8);saved_fd=g_base_fd;CHECK(base_pread(dst,4,2)==4&&!memcmp(dst,"CDEF",4));base_close();CHECK(fcntl(saved_fd,F_GETFD)==-1&&errno==EBADF);
}
static void package_file_lookup(void){
 select_package(NULL,"/controlled/game.nsp");base_open();RomFile files[]={{16,20,"region/movie.webm",0}};rom_files=files;rom_nfiles=1;
 static NxRomfs*reader;static uint64_t off,size;reader=(void*)1;off=size=9;
 CHECK(!romfs_package_file(NULL,&reader,&off,&size));CHECK(!romfs_package_file("region/movie.webm",NULL,&off,&size));
 CHECK(!romfs_package_file("missing",&reader,&off,&size)&&!reader&&!off&&!size);
 CHECK(romfs_package_file("/region/movie.webm",&reader,&off,&size)&&reader&&reader!=g_base_package&&off==16&&size==20);
 base_close();expected_off=off;expected_size=8;uint8_t data[8];plat_commit((uintptr_t)data,8);CHECK(nx_romfs_pread(reader,data,8,off)==8);for(unsigned i=0;i<8;i++)CHECK(data[i]==((i+16)^0x5a));nx_romfs_close(reader);
 base_open();files[0].off=128;files[0].size=1;REJECT(romfs_package_file("region/movie.webm",&reader,&off,&size));CHECK(!reader&&!off&&!size);
 files[0].off=UINT64_MAX;files[0].size=2;REJECT(romfs_package_file("region/movie.webm",&reader,&off,&size));
 files[0].off=16;files[0].size=20;bad_clone=true;REJECT(romfs_package_file("region/movie.webm",&reader,&off,&size));CHECK(strstr(fatal_message,"cannot clone"));bad_clone=false;base_close();
 // A traditional mount does not accidentally claim a package reader.
 select_package(NULL,NULL);base_open();CHECK(!romfs_package_file("region/movie.webm",&reader,&off,&size)&&!reader&&!off&&!size);base_close();rom_files=NULL;rom_nfiles=0;
}
int main(void){
 char root[]="/tmp/shared-package-fs-XXXXXX";CHECK(mkdtemp(root));char folder[256],a[280],b[280],raw[256],raw_file[270];snprintf(folder,sizeof folder,"%s/romfs",root);CHECK(!mkdir(folder,0700));snprintf(a,sizeof a,"%s/alpha",folder);snprintf(b,sizeof b,"%s/beta",folder);snprintf(raw,sizeof raw,"%s/legacy",root);snprintf(raw_file,sizeof raw_file,"%s.bin",raw);write_file(a,"ABCD");write_file(b,"EFGHI");write_file(raw_file,"ABCDEFGH");g_romfs_dir=folder;
 package_reads();read_errors();invalid_package();folder_and_image(folder,raw);g_romfs_dir=folder;package_file_lookup();
 for(unsigned i=0;i<24;i++){g_romfs_dir=folder;select_package(NULL,"/controlled/game.nsp");base_open();base_close();select_package(NULL,NULL);base_open();CHECK(!g_base_package&&g_base.nfiles==2);base_close();}
 CHECK(opens+clones==closes+1&&layout_frees>=24);CHECK(!unlink(a)&&!unlink(b)&&!unlink(raw_file)&&!rmdir(folder)&&!rmdir(root));select_package(NULL,NULL);
 printf("{\"state\":\"passed\",\"checks\":%u,\"package_priority\":true,\"traditional_folder_and_image\":true,\"efault_retries\":true,\"repeated_lifecycle\":24}\n",checks);return 0;
}
