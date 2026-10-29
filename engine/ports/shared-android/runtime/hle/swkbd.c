/* Derived from LAS Recomp Android cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp.
 * UTF-16 conversion adapted from ptichalouf's Raitchi PRs #7/#8; bounded guest
 * snapshots/writes avoid trusting an arbitrary input length or buffer capacity. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../hle.h"
#include "../kernel.h"
#include "../ui/keyboard_text.h"
enum { KEYBOARD_TEXT_BYTES=96,KEYBOARD_CANCELLED=0x29f };
int menu_keyboard_run(const char *initial,int max_len,char *out,int cap);
static bool keyboard_field(void *out,uint64_t base,uint64_t offset,size_t bytes){
 return base<=UINT64_MAX-offset&&kernel_copy_readable(out,base+offset,bytes);
}
static uint16_t keyboard_unit(const uint8_t *bytes,size_t index){return(uint16_t)bytes[index*2]|(uint16_t)bytes[index*2+1]<<8;}
/* Initial text is UTF-16LE in the SDK work buffer in both output modes.
 * Layout basis: nnsdk-rs ShowKeyboardArg (work pointer/bytes +0x4d0/+0x4d8),
 * and nx2android SDK ShowKeyboard compatibility implementation (+0x3bd mode). */
static bool keyboard_read_utf16(char out[KEYBOARD_TEXT_BYTES],uint64_t work,uint64_t work_bytes,uint32_t offset,uint32_t units){
 out[0]=0;if(!units)return true;
 if(!work||work>UINT64_MAX-work_bytes||offset>work_bytes||(uint64_t)units>(work_bytes-offset)/2)return false;
 size_t take=units<KEYBOARD_TEXT_BYTES-1?units:KEYBOARD_TEXT_BYTES-1;
 uint8_t bytes[KEYBOARD_TEXT_BYTES*2]={0};
 if(!keyboard_field(bytes,work,offset,take*2))return false;
 size_t length=0;while(length<take&&keyboard_unit(bytes,length))length++;
 // A bounded prefix may end before a low surrogate; omit that incomplete pair.
 if(length&&length==take&&take<units&&keyboard_unit(bytes,length-1)>=0xd800&&keyboard_unit(bytes,length-1)<=0xdbff)length--;
 for(size_t i=0;i<length;i++){
  uint16_t ch=keyboard_unit(bytes,i);
  if(ch>=0xd800&&ch<=0xdbff){if(i+1>=length)return false;uint16_t low=keyboard_unit(bytes,++i);if(low<0xdc00||low>0xdfff)return false;}
  else if(ch>=0xdc00&&ch<=0xdfff)return false;
 }
 bytes[length*2]=bytes[length*2+1]=0;
 size_t written=0;
 for(size_t i=0;i<length;i++){
  uint32_t cp=keyboard_unit(bytes,i);
  if(cp>=0xd800&&cp<=0xdbff){uint32_t low=keyboard_unit(bytes,++i);cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);}
  size_t n=cp<0x80?1:cp<0x800?2:cp<0x10000?3:4;
  if(n>KEYBOARD_TEXT_BYTES-1-written)break;
  if(n==1)out[written++]=(char)cp;
  else{unsigned shift=(unsigned)((n-1)*6);out[written++]=(char)((n==2?0xc0:n==3?0xe0:0xf0)|(cp>>shift));while(shift){shift-=6;out[written++]=(char)(0x80|((cp>>shift)&63));}}
 }
 out[written]=0;return true;
}
static bool keyboard_write_utf16(uint64_t output,uint64_t capacity,const char *text,int max_len){
 if(!output||capacity<2||output>UINT64_MAX-capacity)return false;
 size_t input=0;while(input<KEYBOARD_TEXT_BYTES&&text[input])input++;if(input==KEYBOARD_TEXT_BYTES)return false;
 // Strictly validate the complete bounded UTF-8 answer before any guest write.
 if(keyboard_text_prefix(text,input,(size_t)-1,0)!=input)return false;
 uint8_t wide[KEYBOARD_TEXT_BYTES*2];size_t units=0;
 uint64_t limit=capacity/2-1;if(max_len>0&&limit>(uint64_t)max_len)limit=(uint64_t)max_len;
 for(size_t i=0;i<input;){
  unsigned char lead=(unsigned char)text[i++];size_t n=lead<0x80?1:lead<0xe0?2:lead<0xf0?3:4;
  uint32_t cp=lead&(n==1?0x7f:n==2?31:n==3?15:7);
  for(size_t k=1;k<n;k++)cp=(cp<<6)|((unsigned char)text[i++]&63);
  size_t need=cp>=0x10000?2:1;if(need>limit-units)break;
  if(need==2){cp-=0x10000;uint16_t hi=(uint16_t)(0xd800|(cp>>10)),lo=(uint16_t)(0xdc00|(cp&1023));wide[units*2]=(uint8_t)hi;wide[units*2+1]=(uint8_t)(hi>>8);units++;wide[units*2]=(uint8_t)lo;wide[units*2+1]=(uint8_t)(lo>>8);units++;}
  else{wide[units*2]=(uint8_t)cp;wide[units*2+1]=(uint8_t)(cp>>8);units++;}
 }
 wide[units*2]=wide[units*2+1]=0;
 // Only the answer and terminator are output; never touch arbitrary capacity.
 return kernel_copy_writable(output,wide,(units+1)*2);
}
static bool keyboard_write_utf8(uint64_t output,uint64_t capacity,const char *text,int max_len){
 if(!output||!capacity||output>UINT64_MAX-capacity)return false;
 size_t input=0;while(input<KEYBOARD_TEXT_BYTES&&text[input])input++;if(input==KEYBOARD_TEXT_BYTES)return false;
 size_t room=capacity-1<KEYBOARD_TEXT_BYTES-1?(size_t)(capacity-1):KEYBOARD_TEXT_BYTES-1;
 size_t bytes=keyboard_text_prefix(text,input,max_len>0?(size_t)max_len:(size_t)-1,0);
 if(bytes>room)bytes=keyboard_text_prefix(text,room,(size_t)-1,0);
 char encoded[KEYBOARD_TEXT_BYTES];memcpy(encoded,text,bytes);encoded[bytes]=0;
 return kernel_copy_writable(output,encoded,bytes+1);
}
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE",swkbd_show){
 uint64_t output=c->x[0],argument=c->x[1],string[2],work[2];int32_t max_len;uint32_t offset,units;uint8_t use_utf8;
 // A rejected/unreadable ABI returns the SDK's cancellation result, without UI
 // or any partial output write. This is a known non-success result, not a guessed
 // invalid-argument result number.
 c->x[0]=KEYBOARD_CANCELLED;
 if(!keyboard_field(string,output,0,sizeof string)||!keyboard_field(&max_len,argument,0x3ac,sizeof max_len)||
    !keyboard_field(&use_utf8,argument,0x3bd,sizeof use_utf8)||
    !keyboard_field(&offset,argument,0x3c0,sizeof offset)||!keyboard_field(&units,argument,0x3c4,sizeof units)||
    !keyboard_field(work,argument,0x4d0,sizeof work)||!string[0]||string[1]<(use_utf8?1u:2u)||string[0]>UINT64_MAX-string[1])return;
 char initial[KEYBOARD_TEXT_BYTES],text[KEYBOARD_TEXT_BYTES]={0};
 if(!keyboard_read_utf16(initial,work[0],work[1],offset,units))return;
 if(ENV("LARECOMP_SWKBD_TRACE"))LOG("[swkbd] encoding_utf8=%u initial_units=%u max_units=%d capacity=%llu\n",!!use_utf8,units,max_len,(unsigned long long)string[1]);
 int accepted=menu_keyboard_run(initial,max_len,text,sizeof text);text[sizeof text-1]=0;
 if(!accepted)snprintf(text,sizeof text,"%s",initial); // Preserve existing cancel/initial behavior; never invent a name.
 bool ok=use_utf8?keyboard_write_utf8(string[0],string[1],text,max_len):keyboard_write_utf16(string[0],string[1],text,max_len);
 if(ok){c->x[0]=0;if(ENV("LARECOMP_SWKBD_TRACE")){size_t result_units=0;size_t bytes=keyboard_text_prefix(text,KEYBOARD_TEXT_BYTES-1,max_len>0?(size_t)max_len:(size_t)-1,&result_units);if(use_utf8&&bytes>=string[1])bytes=keyboard_text_prefix(text,(size_t)string[1]-1,(size_t)-1,&result_units);if(!use_utf8&&result_units>string[1]/2-1){size_t limit=(size_t)(string[1]/2-1);bytes=keyboard_text_prefix(text,KEYBOARD_TEXT_BYTES-1,limit,&result_units);}LOG("[swkbd] encoding_utf8=%u result_bytes=%zu result_units=%zu\n",!!use_utf8,use_utf8?bytes+1:(result_units+1)*2,result_units);}}

}
