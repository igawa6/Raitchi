/* Actual changed input policy and controller-finger body; controlled host input/time. */
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "adapters/title_adapter.h"

static unsigned checks,ticks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static TitleAdapter selected;
const TitleAdapter *title_adapter_selected(void){return &selected;}
static bool menu;
static uint64_t key_buttons,mouse_buttons,pad_buttons,script_buttons,virtual_buttons;
static int32_t pad_rx,pad_ry,script_rx,script_ry;
static uint64_t touch_buttons(void){return virtual_buttons;}
static uint64_t SDL_GetTicksNS(void){ticks++;return UINT64_C(1000000000)+ticks*10000000;}
static bool menu_input_captured(void){return menu;}
static int touch_w(void){return 1920;}
#define TOUCH_H 1080
static double finger_x=960,finger_y=540;
static _Atomic uint64_t finger_seen_ns;
static _Atomic int finger_down;
#include "production_input_policy.inc"

static uint64_t poll(uint64_t tap) {
    uint64_t held=key_buttons|mouse_buttons|pad_buttons|touch_buttons();
    uint64_t b=menu?script_buttons:held|tap|script_buttons;
#include "production_input_button_block.inc"
    return b;
}
int main(void) {
    uint64_t zr=UINT64_C(1)<<BTN_ZR,a=UINT64_C(1)<<BTN_A;
    for(int title=TITLE_LAS;title<=TITLE_ECHOES;title++) {
        selected.id=(enum TitleId)title;
        for(unsigned combinations=0;combinations<32;combinations++) {
            menu=(combinations&1)!=0;pad_buttons=zr|a;key_buttons=(combinations&2)?zr:0;
            virtual_buttons=(combinations&4)?zr:0;mouse_buttons=0;script_buttons=(combinations&8)?zr:0;
            uint64_t tap=(combinations&16)?a:0;
            uint64_t actual=poll(tap);
            bool expected_zr=title==TITLE_EEVEE?(!menu&&(key_buttons||virtual_buttons))
                :(menu?script_buttons!=0:true);
            CHECK(((actual&zr)!=0)==expected_zr);CHECK(((actual&a)!=0)==!menu);
        }
        key_buttons=virtual_buttons=mouse_buttons=0;pad_buttons=zr;script_buttons=0;menu=false;
        int32_t x=-1,y=-1;unsigned before=ticks;pad_rx=12000;pad_ry=0;
        bool down=pad_finger(&x,&y);
        if(title==TITLE_EEVEE) {
            CHECK(down);CHECK(ticks==before+1);CHECK(x>=0&&x<1920&&y>=0&&y<1080);
            CHECK(finger_down==1&&finger_seen_ns!=0);
            menu=true;CHECK(!pad_finger(&x,&y));CHECK(finger_down==0);
            menu=false;pad_buttons=0;CHECK(!pad_finger(&x,&y));
        } else {CHECK(!down);CHECK(ticks==before);CHECK(x==-1&&y==-1);}
    }
    puts("LAS/Eevee/Echoes physical ZR and controller-finger isolation passed");
    printf("%u input policy assertions passed (actual production blocks; controlled input/time)\n",checks);
}
