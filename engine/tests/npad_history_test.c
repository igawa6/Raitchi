/* Real input provider + batch entry points, controlled host inputs and guest callbacks. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include "runtime/aot_cpu.h"
#include "adapters/title_adapter.h"
#include "hle/npad_history.h"
static unsigned checks;
#define CHECK(x) do { ++checks;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();} } while(0)
static void quiet_log(const char *format,...) {(void)format;}
#define LOG(...) quiet_log(__VA_ARGS__)
#define HLE_FUNC(name,fn) static void fn(Cpu *c)
#define RT_ON_RESET(fn) static void fn(void)
static TitleAdapter selected={.id=TITLE_WONDER};
const TitleAdapter *title_adapter_selected(void){return &selected;}
static bool shared_title_las(void){return selected.id==TITLE_LAS;}
static uint32_t policy_id=0x20,policy_style=2;
uint32_t shared_input_npad(uint32_t *style){if(style)*style=policy_style;return policy_id;}
static _Atomic uint64_t key_buttons,pad_buttons,mouse_buttons,press_unseen,tap_buttons;
static uint64_t script_buttons,g_pad_obj;
static volatile int g_inject_plus;
static _Atomic int32_t key_lx,key_ly,key_rx,key_ry,pad_lx,pad_ly,pad_rx,pad_ry;
static int32_t script_lx,script_ly,script_rx,script_ry;
static uint8_t keys_down[512];
static struct {bool dpad_move;} g_settings;
static bool menu;
static uint64_t frame;
static bool menu_input_captured(void){return menu;}
static uint64_t touch_buttons(void){return 0;}
static void touch_stick(int32_t*x,int32_t*y){*x=*y=0;}
bool mirror_input(void){return false;}
uint64_t renderer_present_count(void){return frame;}
uint64_t g_watch_lo,g_watch_hi,g_stick_freeze_until;
int32_t g_stick_lx,g_stick_ly;
static void rumble_clear(void);
static void mouse_release(void){mouse_buttons=0;}
static void recompute_keyboard(void){key_buttons=0;key_lx=key_ly=key_rx=key_ry=0;}
void warp_game_tick(Cpu *c);
#define EMPTY_TICK(name) void name(Cpu*c,uint64_t f){(void)c;(void)f;}
EMPTY_TICK(give_item_tick) EMPTY_TICK(chamber_game_tick) EMPTY_TICK(digs_game_tick)
EMPTY_TICK(pot_bush_game_tick) EMPTY_TICK(shell_spots_tick)
void qol_tick(void){} void run_timer_tick(void){} void pets_game_tick(Cpu*c){(void)c;}
uint64_t pets_menu_input(uint64_t b,int32_t st[4]){(void)st;return b;}
uint64_t tracker_map_input(uint64_t b,int32_t st[4]){(void)st;return b;}
void input_note_rstick(int32_t y){(void)y;}
#include "npad_input_production.inc"
static Cpu cpu;
static uint8_t raw_id[8];
static NpadHistoryState nested[16];
static unsigned reenter,reset_capture,reset_interleave;
static void set_id(uint32_t id){memcpy(raw_id+1,&id,4);}
static void batch(Cpu*c,void*out,int32_t count,uint32_t style){c->x[0]=(uintptr_t)out;c->x[1]=(uint32_t)count;c->x[2]=(uintptr_t)(raw_id+1);fill_states(c,style);}
void warp_game_tick(Cpu *c) {
    if(reset_capture){reset_capture=0;input_reset();}
    if(reenter){reenter=0;uint64_t saved[3]={c->x[0],c->x[1],c->x[2]};batch(c,nested,16,1);CHECK(c->x[0]==1);memcpy(c->x,saved,sizeof saved);}
}
static void rumble_clear(void) {
    if(reset_interleave){reset_interleave=0;batch(&cpu,nested,16,policy_style);CHECK(nested[0].buttons!=0);}
}
static NpadHistoryState state_at(const void *p,unsigned i){NpadHistoryState s;memcpy(&s,(const uint8_t*)p+i*40,40);return s;}
static void zero_bytes(const void*p,size_t n){const uint8_t*b=p;for(size_t i=0;i<n;i++)CHECK(!b[i]);}
static void canary(const uint8_t*p,size_t n){for(size_t i=0;i<n;i++)CHECK(p[i]==0xa5);}
static void batch_edges(void) {
    input_reboot_reset();set_id(policy_id);uint8_t buffer[16*40+34];void*out=buffer+1;
    memset(buffer,0xa5,sizeof buffer);uint64_t before=atomic_load(&sampling);batch(&cpu,out,16,1);
    CHECK(!cpu.x[0] && atomic_load(&sampling)==before);zero_bytes(out,640);canary(buffer,1);canary(buffer+641,sizeof buffer-641);
    const uint64_t buttons[]={1,0,(1u<<6)|(1u<<7),0};NpadHistoryState prior[16]={0};
    for(unsigned i=0;i<4;i++) {
        key_buttons=buttons[i];key_lx=12345;key_ly=-12345;frame++;
        memset(buffer,0xa5,sizeof buffer);batch(&cpu,out,16,2);CHECK(cpu.x[0]==i+1);
        NpadHistoryState s=state_at(out,0);CHECK(s.sampling==i+1 && s.buttons==buttons[i] && s.lx==12345 && s.ly==-12345 && s.attributes==1);
        for(unsigned j=0;j<i;j++){NpadHistoryState older=state_at(out,j+1);CHECK(!memcmp(&older,&prior[j],40));}
        memcpy(prior,out,640);zero_bytes((uint8_t*)out+(i+1)*40,(15-i)*40);canary(buffer,1);canary(buffer+641,sizeof buffer-641);
        CHECK(cpu.x[1]==16 && cpu.x[2]==(uintptr_t)(raw_id+1));
    }
    // Inspect all16 despite count: only real serials can create an input edge.
    unsigned a_edges=0,lr_edges=0;uint64_t old=0;
    for(int i=15;i>=0;i--){NpadHistoryState s=state_at(out,(unsigned)i);if(!s.sampling)continue;uint64_t pressed=s.buttons&~old;a_edges+=(pressed&1)!=0;lr_edges+=(pressed&0xc0)==0xc0;old=s.buttons;}
    CHECK(a_edges==1 && lr_edges==1);
    for(unsigned i=4;i<24;i++){key_buttons=i;frame++;batch(&cpu,out,16,2);CHECK(cpu.x[0]==(i<16?i+1:16));}
    for(unsigned i=0;i<16;i++){NpadHistoryState s=state_at(out,i);CHECK(s.sampling==24-i);}
    memset(buffer,0xa5,sizeof buffer);batch(&cpu,out,1,2);CHECK(cpu.x[0]==1);canary(buffer+41,sizeof buffer-41);
    memset(buffer,0xa5,sizeof buffer);batch(&cpu,out,INT32_MAX,2);CHECK(cpu.x[0]==16);canary(buffer+641,sizeof buffer-641);
    before=atomic_load(&sampling);
    for(int count=-2;count<=0;count++){memset(buffer,0xa5,sizeof buffer);batch(&cpu,out,count,2);CHECK(!cpu.x[0]);canary(buffer,sizeof buffer);}
    batch(&cpu,NULL,16,2);CHECK(!cpu.x[0] && atomic_load(&sampling)==before);
    cpu.x[0]=(uintptr_t)out;cpu.x[1]=16;cpu.x[2]=0;fill_states(&cpu,2);CHECK(!cpu.x[0]);zero_bytes(out,640);CHECK(atomic_load(&sampling)==before);
}
static void style_and_reset(void) {
    NpadHistoryState out[16];policy_id=0;policy_style=1|4;set_id(0);key_buttons=1;batch(&cpu,out,16,1);CHECK(cpu.x[0]==1);NpadHistoryState full=out[0];
    key_buttons=2;batch(&cpu,out,16,4);CHECK(cpu.x[0]==1 && out[0].buttons==2);key_buttons=0;batch(&cpu,out,16,1);CHECK(cpu.x[0]==2 && !memcmp(&out[1],&full,40));
    uint64_t serial=atomic_load(&sampling);set_id(7);batch(&cpu,out,16,1);CHECK(!cpu.x[0] && atomic_load(&sampling)==serial);zero_bytes(out,sizeof out);
    set_id(UINT32_MAX);batch(&cpu,out,16,1);CHECK(!cpu.x[0]);zero_bytes(out,sizeof out);
    policy_id=0x20;policy_style=2;set_id(0x20);key_buttons=4;batch(&cpu,out,16,2);CHECK(cpu.x[0]==1 && out[0].buttons==4);zero_bytes(out+1,15*40);
    // Singular aliases intentionally retain previous ID-only style behavior.
    uint8_t unaligned[42];cpu.x[0]=(uintptr_t)(unaligned+1);cpu.x[1]=(uintptr_t)(raw_id+1);hid_state_fullkey(&cpu);CHECK(state_at(unaligned+1,0).attributes==1);
    reset_interleave=1;input_reset();CHECK(!reset_interleave);batch(&cpu,out,16,2);CHECK(cpu.x[0]==1 && !out[0].buttons);zero_bytes(out+1,15*40);
    selected.id=TITLE_LAS;policy_id=0;policy_style=1;set_id(0);input_reboot_reset();key_buttons=0x80;reenter=1;batch(&cpu,out,16,1);
    CHECK(cpu.x[0]==2 && out[0].sampling==2 && out[1].sampling==1);zero_bytes(out+2,14*40);
    key_buttons=0x40;reset_capture=1;batch(&cpu,out,16,1);CHECK(!cpu.x[0]);zero_bytes(out,sizeof out);
    input_reboot_reset();CHECK(!atomic_load(&sampling));selected.id=TITLE_WONDER;
}
static void all_aliases(void) {
    void (*const functions[5])(Cpu*)={hid_states_fullkey,hid_states_handheld,hid_states_dual,hid_states_left,hid_states_right};
    NpadHistoryState out[16];
    for(unsigned i=0;i<5;i++) {
        policy_id=i==1?0x20:0;policy_style=1u<<i;set_id(policy_id);input_reboot_reset();key_buttons=1u<<i;
        cpu.x[0]=(uintptr_t)out;cpu.x[1]=16;cpu.x[2]=(uintptr_t)(raw_id+1);functions[i](&cpu);
        CHECK(cpu.x[0]==1 && out[0].attributes==1 && out[0].buttons==(1u<<i));zero_bytes(out+1,15*40);
    }
}
static void direct_ordering(void) {
    NpadHistoryBank bank={0};NpadHistoryState out[16],s={.sampling=2,.buttons=1,.attributes=1};
    CHECK(npad_history_read(&bank,0,1,0,1,&s,out,16)==1);s.sampling=1;s.buttons=0;CHECK(npad_history_read(&bank,0,1,0,1,&s,out,16)==2);
    CHECK(out[0].sampling==2 && out[1].sampling==1);s.sampling=2;s.buttons=9;CHECK(npad_history_read(&bank,0,1,0,1,&s,out,16)==2 && out[0].buttons==1);
}
int main(void){batch_edges();style_and_reset();all_aliases();direct_ordering();printf("state=passed checks=%u; actual40B input provider/plural ABI, bounded stablehistory/edges/styles/reset/reentry\n",checks);return 0;}
