/* Actual runtime bridge policy with controlled package-provider stand-ins.
 * No encryption, URI/JNI, FD or read-cache behavior is claimed by this test. */
#define _POSIX_C_SOURCE 200809L
#include "native_package_android.h"
#include "adapters/title_adapter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct NxPackage{uint64_t title;};struct NxRomfs{int owned;};
static struct NxPackage package;static struct NxRomfs reader;
static unsigned checks,opens,closes,extracts,romfs;static int fail_open,fail_extract,fail_romfs;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
NxPackage*nx_package_open(const char*p,const char*k,char*why,size_t cap){CHECK(!strcmp(p,"/owner/game.nsp"));CHECK(!strcmp(k,"/private/prod.keys"));opens++;if(fail_open){snprintf(why,cap,"provider open rejected");return NULL;}return &package;}
uint64_t nx_package_title_id(const NxPackage*p){CHECK(p==&package);return p->title;}
bool nx_package_extract_exefs(NxPackage*p,const char*d,char*why,size_t cap){CHECK(p==&package);CHECK(!strcmp(d,"/private/generation/exefs"));extracts++;if(fail_extract){snprintf(why,cap,"originals differ");return false;}return true;}
NxRomfs*nx_package_open_romfs(const NxPackage*p,char*why,size_t cap){CHECK(p==&package);romfs++;if(fail_romfs){snprintf(why,cap,"RomFS rejected");return NULL;}reader.owned=1;return &reader;}
void nx_package_close(NxPackage*p){CHECK(p==&package);closes++;}
int main(void){
 char why[256];unsetenv("LARECOMP_PACKAGE_URI");unsetenv("LARECOMP_PACKAGE_PATH");unsetenv("LARECOMP_PACKAGE_KEYS");
 CHECK(!shared_package_open_romfs(why,sizeof why));CHECK(opens==0);
 setenv("LARECOMP_PACKAGE_URI","content://provider/game",1);setenv("LARECOMP_PACKAGE_KEYS","/private/prod.keys",1);
 CHECK(!shared_package_open_romfs(why,sizeof why));CHECK(strstr(why,"Android"));CHECK(opens==0);
 setenv("LARECOMP_PACKAGE_URI","/owner/game.nsp",1);setenv("LARECOMP_GAME_ROOT","/private/generation",1);
 CHECK(!shared_package_open_romfs(why,sizeof why));CHECK(opens==1&&closes==1&&extracts==0&&romfs==0);
 CHECK(title_adapter_select(TITLE_LAS)==TITLE_ADAPTER_OK);package.title=title_adapter_find(TITLE_EEVEE)->program_id;
 CHECK(!shared_package_open_romfs(why,sizeof why));CHECK(opens==2&&closes==2&&extracts==0&&romfs==0);
 package.title=title_adapter_selected()->program_id;fail_extract=1;
 CHECK(!shared_package_open_romfs(why,sizeof why));CHECK(!strcmp(why,"originals differ"));CHECK(extracts==1&&romfs==0&&closes==3);
 fail_extract=0;fail_romfs=1;CHECK(!shared_package_open_romfs(why,sizeof why));CHECK(!strcmp(why,"RomFS rejected"));CHECK(extracts==2&&romfs==1&&closes==4);
 fail_romfs=0;NxRomfs*r=shared_package_open_romfs(why,sizeof why);CHECK(r==&reader&&reader.owned);CHECK(extracts==3&&romfs==2&&closes==5);
 // Local path fallback is identical and also keeps reader independent of package cleanup.
 unsetenv("LARECOMP_PACKAGE_URI");setenv("LARECOMP_PACKAGE_PATH","/owner/game.nsp",1);CHECK(shared_package_open_romfs(why,sizeof why)==&reader);CHECK(closes==6);
 fail_open=1;CHECK(!shared_package_open_romfs(why,sizeof why));CHECK(!strcmp(why,"provider open rejected"));CHECK(closes==6);
 printf("%u actual runtime-bridge policy assertions passed (controlled provider)\n",checks);return 0;
}
