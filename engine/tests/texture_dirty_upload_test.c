/* Compile the real upload_texture decision prefix with asset-free memory. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "nvn/nvn.h"
#include "tex_hash.h"
/* Exact current renderer struct; NVN layouts above are the production header. */
#include "production_backend.inc"
typedef struct VkFmt {unsigned vf;} VkFmt;
#define TEXPATCH_RECOLOUR_BASE 1024
static unsigned checks,uploads,full_checks,watch_calls,marker;
static uint64_t frame,uploaded_bytes;
static int dirty;
static uint32_t generation=10,last_since,style_generation;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint64_t vk_frame_counter(void){return frame;}
static VkFmt fmt_of(unsigned format){return (VkFmt){format};}
static unsigned texpatch_marker(NvnTexture *texture){(void)texture;return marker;}
static uint32_t texpatch_generation(void){return style_generation;}
int vk_pages_changed(uint64_t address,uint64_t size,uint32_t since,uint32_t *now){CHECK(address&&size);watch_calls++;last_since=since;*now=++generation;return dirty;}
void slow_note_full_hash(void){full_checks++;}
void slow_note_upload(uint64_t size){uploads++;uploaded_bytes+=size;}
#undef FATAL
#define FATAL(...) do {fprintf(stderr,__VA_ARGS__);abort();} while(0)
#include "production_pool_resolver.inc"
/* Exact helper/hash code and upload decision statements extracted by checker.
 * The Vulkan upload tail is modeled as a successful transfer after
 * slow_note_upload, followed by the actual final generation acknowledgement.
 * No GPU is modeled; decision/hash/dirty-page guards remain production code. */
#include "texture_upload_prefix.inc"
static void next_frame(const TexBackend *backend){frame+=8;frame=(frame&~UINT64_C(7))+(8-(((uintptr_t)backend>>6)&7));}
static void check_sparse(uint64_t size) {
    uint8_t *memory=calloc(1,(size_t)size);CHECK(memory);FormatInfo format={.bpb=1,.bw=1,.bh=1};
    NvnTexture texture={.addr=(uintptr_t)memory,.format=1,.layout={.fmt=&format,.width=1024,.height=size/1024,.depth=1,.layers=1,.levels=1}};TexBackend backend={.format=1};
    CHECK(tegra_total_size(&texture.layout)==size);
    marker=0;style_generation=0;dirty=0;next_frame(&backend);
    unsigned start_uploads=uploads,start_full=full_checks;upload_texture(&texture,&backend);
    CHECK(uploads==start_uploads+1&&full_checks==start_full+1&&backend.uploaded&&!backend.dynamic);
    uint64_t sample=tex_fast_hash()?tex_hash_sample(memory,size):fnv_sample(memory,size),original_hash=backend.data_hash;
    /* Byte17 lies outside all4096 sample windows at1MiB and8MiB. */
    memory[17]=0x4a;CHECK((tex_fast_hash()?tex_hash_sample(memory,size):fnv_sample(memory,size))==sample);
    dirty=1;uint32_t old_generation=backend.wgen;frame+=256;next_frame(&backend);
    /* Even when periodic sampling can never fit the4MiB budget, dirty evidence
     * must authorize a full hash before it is consumed. */
    full_hash_budget_frame=frame;full_hash_budget=0;backend.full_frame=0;
    upload_texture(&texture,&backend);
    CHECK(last_since==old_generation&&backend.wgen==generation);
    CHECK(uploads==start_uploads+2&&full_checks==start_full+2&&backend.data_hash!=original_hash&&backend.dynamic);
    dirty=0;next_frame(&backend);unsigned full_before=full_checks,upload_before=uploads;upload_texture(&texture,&backend);
    CHECK(full_checks==full_before&&uploads==upload_before); /* retain clean skips */
    dirty=1;next_frame(&backend);upload_texture(&texture,&backend);
    CHECK(full_checks==full_before+1&&uploads==upload_before); /* neighboring-page false positive */
    dirty=0;next_frame(&backend);upload_texture(&texture,&backend);CHECK(full_checks==full_before+1&&uploads==upload_before);
    /* Unknown tracking retains the previous sample fallback. A later positive
     * result must still detect the same sparse edit and upgrade to dynamic. */
    backend.dynamic=false;backend.sample_hash=tex_fast_hash()?tex_hash_sample(memory,size):fnv_sample(memory,size);
    memory[33]=0xb7;dirty=-1;old_generation=backend.wgen;next_frame(&backend);full_hash_budget_frame=frame;full_hash_budget=0;
    upload_texture(&texture,&backend);CHECK(backend.wgen==old_generation&&uploads==upload_before);
    dirty=1;next_frame(&backend);upload_texture(&texture,&backend);CHECK(uploads==upload_before+1&&backend.dynamic);
    /* Restyling can upload unchanged contents on a clean tracked page. */
    backend.dynamic=false;backend.tp=1;backend.tp_gen=0;style_generation=1;dirty=0;next_frame(&backend);upload_before=uploads;upload_texture(&texture,&backend);
    CHECK(uploads==upload_before+1&&backend.tp_gen==1);
    unsigned watched=watch_calls;upload_texture(&texture,&backend);CHECK(watch_calls==watched); /* same-frame skip */
    texture.addr=0;next_frame(&backend);upload_texture(&texture,&backend);CHECK(watch_calls==watched);
#ifdef TEXTURE_EXPLICIT_CPU_WRITES
    texture.addr=(uintptr_t)memory;backend.dynamic=false;backend.checked_frame=frame;dirty=0;
    memory[55]^=0x6d;__atomic_add_fetch(&texture.cpu_write_generation,1,__ATOMIC_RELEASE);
    upload_before=uploads;full_before=full_checks;watched=watch_calls;
    upload_texture(&texture,&backend);
    CHECK(uploads==upload_before+1&&full_checks==full_before+1);CHECK(watch_calls==watched);
    // A new generation of identical bytes is acknowledged by the actual hash
    // equality path; it does not need a second transfer or dirty-page query.
    __atomic_add_fetch(&texture.cpu_write_generation,1,__ATOMIC_RELEASE);
    upload_texture(&texture,&backend);CHECK(uploads==upload_before+1&&watch_calls==watched);
    CHECK(backend.cpu_write_generation==texture.cpu_write_generation);
#endif
    free(memory);
}
static void check_pool_source(void) {
    uint8_t memory[4096]={0};FormatInfo format={.bpb=1,.bw=1,.bh=1};
    NvnPool pool={.storage=(uintptr_t)memory,.size=sizeof memory};
    NvnTexture texture={.pool=&pool,.format=1,.layout={.fmt=&format,.width=64,.height=64,.depth=1,.layers=1,.levels=1}};
    TexBackend backend={.format=1};marker=0;style_generation=0;dirty=0;next_frame(&backend);
    uint64_t address=99,gen=99;CHECK(!nvn_pool_resolve_span(&pool,1,sizeof memory,&address,&gen));CHECK(!address&&!gen);
    unsigned before=uploads;upload_texture(&texture,&backend);CHECK(uploads==before+1);
#ifdef TEXTURE_EXPLICIT_CPU_WRITES
    CHECK(backend.source_address==(uintptr_t)memory&&backend.source_generation==0);
#endif
}
int main(int argc,char **argv){CHECK(argc==2);CHECK(!unsetenv("LARECOMP_TEX_FULLHASH"));CHECK(!unsetenv("LARECOMP_VK_TEX_WATCH"));CHECK(!setenv("LARECOMP_TEX_FASTHASH",argv[1],1));
    check_sparse(1u<<20);check_sparse(8u<<20);check_pool_source();
    printf("{\"state\":\"passed\",\"checks\":%u,\"fast_hash\":%s,\"uploads\":%u,\"full_hashes\":%u,\"uploaded_bytes\":%llu,\"sparse_edit_missed_by_sample\":true,\"large_atlas_exceeds_periodic_budget\":true,\"actual_decision_prefix\":true}\n",checks,tex_fast_hash()?"true":"false",uploads,full_checks,(unsigned long long)uploaded_bytes);return 0;
}
