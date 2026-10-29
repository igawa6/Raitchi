/* UTF-8 editing boundaries shared by the SDK keyboard and its overlay. */
#pragma once
#include <stddef.h>
#include <stdint.h>
/* Returns a valid prefix bounded by bytes AND UTF-16 code units. Invalid or
 * incomplete UTF-8 terminates the prefix; never emit a partial code point. */
static inline size_t keyboard_text_prefix(const char *s,size_t bytes,size_t max_units,size_t *units_out){
 size_t i=0,units=0;
 while(i<bytes&&s[i]){
  unsigned char a=(unsigned char)s[i];size_t n;uint32_t cp;
  if(a<0x80){n=1;cp=a;}else if(a>=0xc2&&a<=0xdf){n=2;cp=a&31;}else if(a>=0xe0&&a<=0xef){n=3;cp=a&15;}else if(a>=0xf0&&a<=0xf4){n=4;cp=a&7;}else break;
  if(n>bytes-i)break;
  size_t k;for(k=1;k<n;k++){unsigned char b=(unsigned char)s[i+k];if((b&0xc0)!=0x80)break;cp=(cp<<6)|(b&63);}if(k!=n)break;
  if((n==2&&cp<0x80)||(n==3&&cp<0x800)||(n==4&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))break;
  size_t u=cp>=0x10000?2:1;if(u>max_units-units)break;units+=u;i+=n;
 }
 if(units_out)*units_out=units;return i;
}
static inline size_t keyboard_text_trim(char *s,size_t cap,size_t max_units){
 if(!cap)return 0;size_t bytes=keyboard_text_prefix(s,cap-1,max_units,0);s[bytes]=0;return bytes;
}
static inline size_t keyboard_text_units(const char *s,size_t cap){size_t units=0;keyboard_text_prefix(s,cap,(size_t)-1,&units);return units;}
static inline size_t keyboard_text_delete(char *s,size_t cap){
 size_t len=keyboard_text_trim(s,cap,(size_t)-1);if(len){do{len--;}while(len&&((unsigned char)s[len]&0xc0)==0x80);s[len]=0;}return len;
}
