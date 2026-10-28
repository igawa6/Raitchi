/* Actual adapter/accessor/texture policy bodies, no GPU or owned assets. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adapters/title_adapter.h"
#include "nvn/nvn.h"
static unsigned checks,level_reads;static uint32_t g_level_gen=1;
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"line%d %s\n",__LINE__,#x);abort();}}while(0)
static const char*game_level_name(void){level_reads++;return "TelephoneBoxTest";}
#define __ANDROID__ 1
#include "production_renderer_policy.inc"
int main(int argc,char**argv){CHECK(argc==2);enum TitleId id=atoi(argv[1]);
 CHECK(!shared_renderer_legacy_target_policy());CHECK(!title_adapter_selected());
 NvnTexture wide={.target=1,.samples=1,.flags=8,.width=1920,.height=1080};
 CHECK(!tex_scalable(&wide));vk_level_change_check();CHECK(level_reads==0&&g_level_gen==1);
 for(int i=TITLE_LAS;i<=TITLE_WONDER;i++){const TitleAdapter*a=title_adapter_find(i);CHECK(a);CHECK(a->renderer_policy_flags==(i==TITLE_LAS?TITLE_RENDERER_LEGACY_TARGET_HEURISTICS:0));}
 CHECK(title_adapter_select(id)==TITLE_ADAPTER_OK);bool legacy=id==TITLE_LAS;CHECK(shared_renderer_legacy_target_policy()==legacy);
 CHECK(tex_scalable(&wide)==legacy);wide.flags=0;CHECK(!tex_scalable(&wide));wide.flags=8;wide.target=2;CHECK(!tex_scalable(&wide));
 NvnTexture depth={.target=1,.samples=1,.format=0x35,.width=2048,.height=2048};CHECK(shadow_scale(&depth)==(legacy?0.5f:1.0f));CHECK(tex_scalable(&depth)==legacy);
 vk_level_change_check();CHECK(level_reads==(legacy?1:0));CHECK(g_level_gen==(legacy?2u:1u));
 // Clearing compression cannot remove legitimate render capability: this
 // interim policy deliberately leaves all nonlegacy targets at guest size.
 for(unsigned f=0;f<16;f++){wide.target=1;wide.flags=f;CHECK(tex_scalable(&wide)==(legacy&&!!(f&8)));}
 printf("%u renderer-policy checks passed title%d\n",checks,id);return 0;}
