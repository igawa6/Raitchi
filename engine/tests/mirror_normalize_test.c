/* Actual MIR allocator/merge/recorder plus actual conservative admission. */
#define main previous_mirror_main
#include "graphics_mirror_test.c"
#undef main
#include <unistd.h>
#include <sys/wait.h>
static unsigned flushes;
static uint32_t *later_cpu_edit;
static uint32_t later_cpu_value;
static unsigned char uniform_forward[16];
static bool uniform_forward_graphics_prepared;
static uint64_t gcopy_generation;
static void guest_copy_flush(void){gcopy_generation++;}
static uint64_t SDL_GetTicksNS(void){static uint64_t clock;return clock+=1000000;}
static void renderer_flush(void){
 CHECK(pthread_mutex_trylock(&mir_lock)==0);CHECK(!pthread_mutex_unlock(&mir_lock));CHECK(!in_pass);
 execute();done=pending;vk_mirror_writeback();if(later_cpu_edit){*later_cpu_edit=later_cpu_value;later_cpu_edit=NULL;}pending++;flushes++;
}
#include "mirror_normalize.h"
static unsigned active(void){unsigned n=0;for(unsigned i=0;i<MIR_SLOTS;i++)n+=mir_slot[i].used;return n;}
static MirSlot*owner(uint64_t a,uint64_t n){MirSlot*out=NULL;for(unsigned i=0;i<MIR_SLOTS;i++)if(mir_slot[i].used&&mir_slot[i].lo<=a&&a+n<=mir_slot[i].hi){CHECK(!out);out=&mir_slot[i];}CHECK(out);return out;}
static uint32_t gpu(MirSlot*m,unsigned b){uint32_t v;memcpy(&v,mir_ptr+m->off+b,4);return v;}
static void pending_write(MirSlot*m,unsigned offset,uint32_t v){append((Op){.kind=OP_DRAW,.write=m->off+offset,.value=v});mir_stored[0]=m;mir_nstored=1;mirror_after_dispatch(1);}
static void ranges_and_edits(void){
 uint64_t epoch_before=uniform_forward_write_epoch;MirRange first={HEAP_BASE,HEAP_BASE+320,false};mirror_normalize_ranges(&first,1);CHECK(active()==1&&!flushes&&uniform_forward_write_epoch>epoch_before);MirSlot*a=owner(HEAP_BASE,320);uint64_t oldoff=a->off;
 pending_write(a,128,0xface);pending_write(a,268,99);pending_write(a,264,999);uint32_t zero=0;VkSpan explicit=vk_transient_copy(&zero,4,16);VkBufferCopy update={explicit.off,a->off+264,4};rec_CmdCopyBuffer(1,explicit.buf,mir_buf,1,&update);mir_stored[0]=a;mir_nstored=1;mirror_after_dispatch(1);guest[32]=7; /* actual merge would erase this CPU edit */
 MirRange second={HEAP_BASE+256,HEAP_BASE+576,false};in_pass=true;mirror_normalize_ranges(&second,1);CHECK(!in_pass&&flushes==1&&nops==0);CHECK(active()==1);a=owner(HEAP_BASE,576);CHECK(gpu(a,128)==7&&guest[32]==7&&gpu(a,268)==99&&guest[67]==99);CHECK(gpu(a,264)==0&&guest[66]==0);CHECK(!memcmp(a->base,guest,576));CHECK(a->last_use==pending);CHECK(gcopy_generation==1&&!uniform_forward_graphics_prepared);CHECK(done<pending);
 uint64_t stable=a->off;mirror_normalize_ranges(&second,1);MirRange read={HEAP_BASE+260,HEAP_BASE+300,true};mirror_normalize_ranges(&read,1);CHECK(flushes==1&&active()==1&&owner(HEAP_BASE,576)->off==stable);
 /* Repeated expansion preserves an earlier GPU result and a same-submission CPU override. */
 pending_write(a,260,55);pending_write(a,560,123);guest[65]=8;later_cpu_edit=&guest[65];later_cpu_value=9;MirRange third={HEAP_BASE+512,HEAP_BASE+832,false};mirror_normalize_ranges(&third,1);CHECK(flushes==2);a=owner(HEAP_BASE,832);CHECK(gpu(a,128)==7&&gpu(a,268)==99&&gpu(a,260)==9&&gpu(a,560)==123);CHECK(guest[65]==9);CHECK(!memcmp(a->base,guest,832));
 /* Two unique GPU owners bridged by a new range retain their CPU gap. */
 MirRange far={HEAP_BASE+1024,HEAP_BASE+1152,false};mirror_normalize_ranges(&far,1);CHECK(active()==2&&flushes==2);MirSlot*b=owner(HEAP_BASE+1024,128);pending_write(b,16,77);guest[220]=42;MirRange bridge={HEAP_BASE+768,HEAP_BASE+1200,false};mirror_normalize_ranges(&bridge,1);CHECK(active()==1&&flushes==3);a=owner(HEAP_BASE,1200);CHECK(gpu(a,1040)==77&&gpu(a,880)==42&&gpu(a,560)==123&&gpu(a,260)==9);CHECK(!memcmp(a->base,guest,1200));
 CHECK(oldoff<MIR_SIZE&&mirror_normalize_count==3&&mirror_normalize_wait_ns==3000000);cleanup();
}
static void operation_preflight(void){
 unsigned before=flushes;bound(5,0,0,80);bound(5,1,64,80);st.ubuf[5][0]=(Range){HEAP_BASE,1136};mirror_normalize_compute(&writer);CHECK(active()==1&&flushes==before);MirSlot*m=owner(HEAP_BASE,1136);uint32_t tab[(GMAP_MAX+1)*4];mirror_table(5,true,1,tab);CHECK(mir_nstored==1&&mir_stored[0]==m);CHECK(tab[GMAP_MAX*4]==1);CHECK((uint64_t)tab[2]*4==m->off);mir_nstored=0;cleanup();
 bound(0,0,0,80);bound(1,0,64,80);mirror_normalize_graphics(&writer,&writer);CHECK(active()==1&&flushes==before);m=owner(HEAP_BASE,576);mirror_prepare_graphics(&writer,&writer);CHECK(mir_nstored==1&&mir_stored[0]==m&&offset(0,HEAP_BASE+268)==offset(1,HEAP_BASE+268));begin_pass();draw_value(1,HEAP_BASE+268,66);mirror_finish_graphics(1);submit_merge();CHECK(guest[67]==66);cleanup();
}
static void unrelated_merge_cases(bool reserve_only){
 unsigned before=flushes;MirRange pair[]={{HEAP_BASE,HEAP_BASE+320,false},{HEAP_BASE+1024,HEAP_BASE+1152,false}};mirror_normalize_ranges(pair,2);MirSlot*b=owner(HEAP_BASE+1024,128);pending_write(b,0,9);guest[256]=7;
 if(reserve_only){execute();done=pending;pending++;MirRange fresh={HEAP_BASE+2048,HEAP_BASE+2112,false};mirror_normalize_ranges(&fresh,1);CHECK(flushes==before);}
 else{MirRange grow={HEAP_BASE+256,HEAP_BASE+576,false};mirror_normalize_ranges(&grow,1);CHECK(flushes==before+1);}
 b=owner(HEAP_BASE+1024,128);CHECK(guest[256]==7&&((uint32_t*)b->base)[0]==9);
 mirror_cpu_changes(b,1);CHECK(((uint32_t*)b->base)[0]==7);mir_stored[0]=b;mir_nstored=1;mirror_after_dispatch(1);submit_merge();CHECK(guest[256]==7&&gpu(b,0)==7);
 pending_write(b,0,42);submit_merge();CHECK(guest[256]==42&&((uint32_t*)b->base)[0]==42);cleanup();
}
static void event_chronology(void){
 MirRange r={HEAP_BASE,HEAP_BASE+64,false};mirror_normalize_ranges(&r,1);MirSlot*m=owner(HEAP_BASE,64);
 pending_write(m,0,9);execute();uint64_t old=pending;pending++;guest[0]=7;mirror_cpu_changes(m,1);mir_stored[0]=m;mir_nstored=1;mirror_after_dispatch(1);execute();done=old;vk_mirror_writeback();
 CHECK(guest[0]==7&&((uint32_t*)m->base)[0]==7);int newer=-1;for(int j=0;j<MIR_RB;j++)if(m->rb_seq[j]==pending)newer=j;CHECK(newer>=0&&((uint32_t*)m->rb_base[newer])[0]==7);
 pending_write(m,0,42);submit_merge();CHECK(guest[0]==42&&((uint32_t*)m->base)[0]==42);cleanup();
 /* Same pending readback area must rebase after the ordered CPU7 upload. */
 mirror_normalize_ranges(&r,1);m=owner(HEAP_BASE,64);pending_write(m,0,9);guest[0]=7;mirror_cpu_changes(m,1);pending_write(m,0,42);submit_merge();CHECK(guest[0]==42&&((uint32_t*)m->base)[0]==42);cleanup();
 /* Repeated unchanged GPU9 snapshots cannot resurrect9 over a later CPU7 edit. */
 mirror_normalize_ranges(&r,1);m=owner(HEAP_BASE,64);pending_write(m,0,9);execute();pending++;mir_stored[0]=m;mir_nstored=1;mirror_after_dispatch(1);execute();guest[0]=7;done=pending;vk_mirror_writeback();CHECK(guest[0]==7&&((uint32_t*)m->base)[0]==9);for(int j=0;j<MIR_RB;j++)CHECK(!m->rb_seq[j]);pending++;mirror_cpu_changes(m,1);mir_stored[0]=m;mir_nstored=1;mirror_after_dispatch(1);submit_merge();CHECK(guest[0]==7&&gpu(m,0)==7);pending_write(m,0,42);submit_merge();CHECK(guest[0]==42&&((uint32_t*)m->base)[0]==42);cleanup();
 /* Pure GPU chronology still advances0 ->9 ->42 across two pending snapshots. */
 mirror_normalize_ranges(&r,1);m=owner(HEAP_BASE,64);pending_write(m,0,9);execute();pending++;pending_write(m,0,42);execute();done=pending;vk_mirror_writeback();CHECK(guest[0]==42&&((uint32_t*)m->base)[0]==42);cleanup();
}
static void invalid_existing_overlap(void){
 pid_t pid=fork();CHECK(pid>=0);if(!pid){freopen("/dev/null","w",stderr);fixture_uniform_forward=true;mirror_init();pthread_mutex_lock(&mir_lock);mirror_normalize_allocate(HEAP_BASE,HEAP_BASE+320,NULL);mirror_normalize_allocate(HEAP_BASE+256,HEAP_BASE+576,NULL);pthread_mutex_unlock(&mir_lock);MirRange r={HEAP_BASE,HEAP_BASE+320,false};mirror_normalize_ranges(&r,1);_exit(0);}int status;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)!=0);
}
int main(void){MirRange r={HEAP_BASE,HEAP_BASE+320,false};mirror_normalize_ranges(&r,1);CHECK(!mir_buf&&!active()&&!flushes);fixture_uniform_forward=true;ranges_and_edits();operation_preflight();unrelated_merge_cases(false);unrelated_merge_cases(true);event_chronology();invalid_existing_overlap();printf("{\"state\":\"passed\",\"checks\":%u,\"overlap_flushes\":%u,\"disabled_path\":true,\"saved_cpu_edits\":true}\n",checks,flushes);return 0;}
