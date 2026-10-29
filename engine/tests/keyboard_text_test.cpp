#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "keyboard_text.h"
static unsigned checks;
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static char g_kbd_text[96];static int g_kbd_max;static std::atomic<int>g_kbd;static bool g_rt_unwind;
static void input_reset(){}
static uint64_t SDL_GetTicks(){return 0;}
static void SDL_Delay(uint32_t){}
extern "C" void backend_represent(){g_kbd=2;}
#include "keyboard_run.inc"
static void edit(char*text,size_t cap,int max_chars,bool erase,bool append){
#include "keyboard_edit.inc"
 (void)r;if(erase)del();if(append)add('Q');
}
int main(){
 const char *s="A\xc3\xa9\xf0\x9f\x98\x80Z";
 for(size_t bytes=0;bytes<12;bytes++)for(size_t units=0;units<8;units++){
  size_t u=0,n=keyboard_text_prefix(s,bytes,units,&u);CHECK(n<=bytes&&u<=units);CHECK(n==0||n==1||n==3||n==7||n==8);
 }
 char b[96];snprintf(b,sizeof b,"%s",s);CHECK(keyboard_text_delete(b,sizeof b)==7);CHECK(keyboard_text_delete(b,sizeof b)==3);CHECK(keyboard_text_delete(b,sizeof b)==1);CHECK(keyboard_text_delete(b,sizeof b)==0);CHECK(keyboard_text_delete(b,sizeof b)==0);
 snprintf(b,sizeof b,"%s",s);edit(b,sizeof b,4,true,false);CHECK(!strcmp(b,"A\xc3\xa9"));edit(b,sizeof b,2,false,true);CHECK(!strcmp(b,"A\xc3\xa9"));edit(b,sizeof b,3,false,true);CHECK(!strcmp(b,"A\xc3\xa9Q"));
 char bad[]={'A',(char)0xc0,(char)0xaf,0};CHECK(keyboard_text_trim(bad,sizeof bad,20)==1);
 char out[96];char longname[81];memset(longname,'X',80);longname[80]=0;unsetenv("LARECOMP_KBD_AUTO");
 CHECK(menu_keyboard_run(longname,60,out,sizeof out));CHECK(strlen(out)==60&&g_kbd_max==60);
 CHECK(menu_keyboard_run(s,3,out,sizeof out));CHECK(!strcmp(out,"A\xc3\xa9"));
 CHECK(menu_keyboard_run(s,4,out,6));CHECK(!strcmp(out,"A\xc3\xa9"));
 CHECK(menu_keyboard_run(longname,200,out,sizeof out));CHECK(strlen(out)==80&&g_kbd_max==95);
 CHECK(menu_keyboard_run(longname,0,out,sizeof out));CHECK(strlen(out)==80);
 setenv("LARECOMP_KBD_AUTO",s,1);CHECK(menu_keyboard_run("",3,out,sizeof out));CHECK(!strcmp(out,"A\xc3\xa9"));unsetenv("LARECOMP_KBD_AUTO");
 printf("PASS %u keyboard UTF16-unit/codepoint boundaries and extracted menu/OSK assertions\n",checks);return 0;
}
