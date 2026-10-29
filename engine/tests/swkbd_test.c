#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include SWKBD_SOURCE
static unsigned checks,ui_calls,write_calls;static uint64_t bytes_written,max_read;static const char*answer="";static const char*expected_initial="";static bool confirmed=true;
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
typedef struct{uint64_t address,size;bool read,write;}Span;static Span spans[8];static unsigned nspans;
static bool allowed(uint64_t address,uint64_t size,bool write){if(!address||!size||address>UINT64_MAX-size)return false;for(unsigned i=0;i<nspans;i++)if(address>=spans[i].address&&size<=spans[i].size&&address-spans[i].address<=spans[i].size-size&&(write?spans[i].write:spans[i].read))return true;return false;}
bool kernel_copy_readable(void*out,uint64_t address,uint64_t size){if(!allowed(address,size,false))return false;if(size>max_read)max_read=size;memcpy(out,(const void*)(uintptr_t)address,(size_t)size);return true;}
bool kernel_copy_writable(uint64_t address,const void*source,uint64_t size){if(!allowed(address,size,true))return false;write_calls++;bytes_written+=size;memcpy((void*)(uintptr_t)address,source,(size_t)size);return true;}
int menu_keyboard_run(const char*initial,int max_len,char*out,int cap){(void)max_len;ui_calls++;CHECK(!strcmp(initial,expected_initial));if(confirmed)snprintf(out,(size_t)cap,"%s",answer);return confirmed;}
static uint8_t argument[0x4e0],work[2048],output[256];static uint64_t string[2];
static void put32(unsigned offset,uint32_t value){memcpy(argument+offset,&value,sizeof value);}
static void put64(unsigned offset,uint64_t value){memcpy(argument+offset,&value,sizeof value);}
static void span(void*pointer,size_t size,bool read,bool write){CHECK(nspans<8);spans[nspans++]=(Span){(uintptr_t)pointer,size,read,write};}
static Cpu setup(uint64_t capacity,int max_len){nspans=0;ui_calls=write_calls=0;bytes_written=max_read=0;memset(argument,0,sizeof argument);memset(work,0,sizeof work);memset(output,0xa5,sizeof output);string[0]=(uintptr_t)(output+2);string[1]=capacity;put32(0x3ac,(uint32_t)max_len);put64(0x4d0,(uintptr_t)work);put64(0x4d8,sizeof work);span(argument,sizeof argument,true,false);span(work,sizeof work,true,false);span(string,sizeof string,true,false);span(output+2,sizeof output-4,true,true);expected_initial="";answer="";confirmed=true;Cpu c={0};for(unsigned i=0;i<32;i++)c.x[i]=0x123400+i;c.x[0]=(uintptr_t)string;c.x[1]=(uintptr_t)argument;c.pc=77;c.flags=0xabc;return c;}
static void invoke(Cpu*c,uint64_t result){Cpu expected=*c;expected.x[0]=result;swkbd_show(c);CHECK(!memcmp(c,&expected,sizeof *c));}
static void initial(const uint16_t*units,size_t count){put32(0x3c0,14);put32(0x3c4,(uint32_t)count);for(size_t i=0;i<count;i++){work[14+i*2]=(uint8_t)units[i];work[15+i*2]=(uint8_t)(units[i]>>8);}}
static void expect(const uint16_t*units,size_t count,uint64_t cap){CHECK(bytes_written==(count+1)*2&&bytes_written<=cap&&write_calls==1);for(size_t i=0;i<count;i++){CHECK(output[2+i*2]==(uint8_t)units[i]);CHECK(output[3+i*2]==(uint8_t)(units[i]>>8));}CHECK(!output[2+count*2]&&!output[3+count*2]);CHECK(output[0]==0xa5&&output[1]==0xa5);for(size_t i=2+(count+1)*2;i<sizeof output;i++)CHECK(output[i]==0xa5);}
static void basic(void){Cpu c=setup(32,12);answer="Alexandre";const uint16_t alex[]={'A','l','e','x','a','n','d','r','e'};invoke(&c,0);expect(alex,9,32);
 const uint16_t acc[]={0xc9,'l',0xe9,'o','n','o','r','e'};c=setup(32,12);answer="\xc3\x89l\xc3\xa9onore";invoke(&c,0);expect(acc,8,32);
 c=setup(32,3);answer="\xc3\x89l\xc3\xa9onore";invoke(&c,0);expect(acc,3,32);
 c=setup(32,3);answer="A\xf0\x9f\x98\x80Z";const uint16_t surrogate[]={'A',0xd83d,0xde00};invoke(&c,0);expect(surrogate,3,32);
 c=setup(32,2);answer="A\xf0\x9f\x98\x80Z";invoke(&c,0);expect(surrogate,1,32);
 c=setup(7,12);answer="Alexandre";invoke(&c,0);expect(alex,2,7);
 for(unsigned cap=2;cap<=3;cap++){c=setup(cap,12);answer="A";invoke(&c,0);expect(alex,0,cap);}
 c=setup(1,12);invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls&&output[2]==0xa5);
 c=setup(0,12);invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
 c=setup(32,12);answer="ABCDEFGHIJKLmore";const uint16_t exact[]={'A','B','C','D','E','F','G','H','I','J','K','L'};invoke(&c,0);expect(exact,12,32);
 c=setup(32,12);initial(acc,8);expected_initial="\xc3\x89l\xc3\xa9onore";confirmed=false;answer="unused";invoke(&c,0);expect(acc,8,32);
 c=setup(32,12);confirmed=false;invoke(&c,0);expect(NULL,0,32);
 c=setup(32,12);put64(0x4d0,0);put64(0x4d8,0);answer="A";invoke(&c,0);expect(alex,1,32);
 // A huge claimed capacity does not impose an invented forty-unit limit.
 c=setup(1u<<20,60);char long_answer[61];uint16_t long_units[60];memset(long_answer,'Q',60);long_answer[60]=0;for(unsigned i=0;i<60;i++)long_units[i]='Q';answer=long_answer;invoke(&c,0);expect(long_units,60,1u<<20);
 // Reused output: a shorter UTF-16 answer terminates before old trailing data.
 c=setup(32,12);answer="Alexandre";invoke(&c,0);uint8_t tail[sizeof output];memcpy(tail,output,sizeof tail);c.x[0]=(uintptr_t)string;answer="Bob";write_calls=0;bytes_written=0;invoke(&c,0);CHECK(bytes_written==8&&output[2]=='B'&&output[4]=='o'&&output[6]=='b'&&!output[8]&&!output[9]);CHECK(!memcmp(output+10,tail+10,sizeof output-10));
}
static void bounds(void){Cpu c=setup(32,12);put32(0x3c0,sizeof work-1);put32(0x3c4,1);invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
 c=setup(32,12);put32(0x3c4,UINT32_MAX);invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
 c=setup(32,12);put32(0x3c4,1);put64(0x4d0,UINT64_MAX-8);put64(0x4d8,16);invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
 c=setup(32,12);put32(0x3c4,1);put64(0x4d0,1);invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
 c=setup(32,12);const uint16_t invalid[]={0xdc00};initial(invalid,1);invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
 c=setup(32,12);const uint16_t high[]={0xd800};initial(high,1);invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
 c=setup(UINT64_MAX,12);invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
 c=setup(32,12);string[0]=0;invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
 c=setup(32,12);spans[3].write=false;answer="A";invoke(&c,KEYBOARD_CANCELLED);CHECK(ui_calls==1&&!write_calls&&output[2]==0xa5);
 c=setup(32,12);spans[0].size=4;invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
 // Large declared initial text consumes a bounded prefix, never an unbounded allocation/read.
 c=setup(32,12);uint16_t many[600];for(unsigned i=0;i<600;i++)many[i]='X';initial(many,600);char wanted[96];memset(wanted,'X',95);wanted[95]=0;expected_initial=wanted;answer="A";invoke(&c,0);CHECK(max_read<=190&&bytes_written==4);
 c=setup(32,12);uint16_t cut[96];for(unsigned i=0;i<94;i++)cut[i]='A';cut[94]=0xd83d;cut[95]=0xde00;initial(cut,96);memset(wanted,'A',94);wanted[94]=0;expected_initial=wanted;answer="B";invoke(&c,0);CHECK(max_read<=190);
 c=setup(32,12);uint16_t cjk[40];for(unsigned i=0;i<40;i++)cjk[i]=0x4e00;initial(cjk,40);char chinese[94];for(unsigned i=0;i<31;i++)memcpy(chinese+i*3,"\xe4\xb8\x80",3);chinese[93]=0;expected_initial=chinese;answer="C";invoke(&c,0);
}
static void utf8_modes(void){
 const char *value="A\xc3\xa9\xf0\x9f\x98\x80Z";
 for(unsigned cap=1;cap<=12;cap++)for(int limit=1;limit<=6;limit++){
  Cpu c=setup(cap,limit);argument[0x3bd]=1;answer=value;invoke(&c,0);
  size_t n=keyboard_text_prefix(value,strlen(value),(size_t)limit,0);if(n>=cap)n=keyboard_text_prefix(value,cap-1,(size_t)-1,0);
  CHECK(bytes_written==n+1&&!memcmp(output+2,value,n)&&output[2+n]==0);for(size_t i=3+n;i<sizeof output;i++)CHECK(output[i]==0xa5);
 }
 Cpu c=setup(32,12);argument[0x3bd]=1;const uint16_t initial_value[]={0xc9,0xd83d,0xde00};initial(initial_value,3);expected_initial="\xc3\x89\xf0\x9f\x98\x80";confirmed=false;invoke(&c,0);CHECK(bytes_written==7&&!strcmp((char*)output+2,expected_initial));
 c=setup(32,12);argument[0x3bd]=1;answer="Alexandre";invoke(&c,0);uint8_t tail[sizeof output];memcpy(tail,output,sizeof tail);c.x[0]=(uintptr_t)string;answer="B";invoke(&c,0);CHECK(bytes_written==12&&output[2]=='B'&&!output[3]&&!memcmp(output+4,tail+4,sizeof output-4));
 c=setup(0,12);argument[0x3bd]=1;invoke(&c,KEYBOARD_CANCELLED);CHECK(!ui_calls&&!write_calls);
}
static void scalar_conversion(void){
 const uint32_t scalar[]={0x7f,0x80,0x7ff,0x800,0xd7ff,0xe000,0xffff,0x10000,0x1ffff,0x10ffff};
 for(size_t j=0;j<sizeof scalar/sizeof scalar[0];j++){
  uint32_t cp=scalar[j];char text[5]={0};size_t n=cp<0x80?1:cp<0x800?2:cp<0x10000?3:4;
  if(n==1)text[0]=(char)cp;else{unsigned shift=(unsigned)((n-1)*6);text[0]=(char)((n==2?0xc0:n==3?0xe0:0xf0)|(cp>>shift));for(size_t k=1;k<n;k++){shift-=6;text[k]=(char)(0x80|((cp>>shift)&63));}}
  uint16_t wide[2];size_t count=1;if(cp>=0x10000){uint32_t v=cp-0x10000;wide[0]=(uint16_t)(0xd800|(v>>10));wide[1]=(uint16_t)(0xdc00|(v&1023));count=2;}else wide[0]=(uint16_t)cp;
  for(unsigned cap=2;cap<10;cap++)for(int limit=1;limit<=3;limit++){
   Cpu c=setup(cap,limit);answer=text;invoke(&c,0);size_t expected=count;if(expected>cap/2-1||expected>(size_t)limit)expected=0;expect(wide,expected,cap);
  }
  Cpu c=setup(32,12);initial(wide,count);expected_initial=text;answer="X";invoke(&c,0);CHECK(bytes_written==4);
 }
 const char *bad[]={"\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82","\x80"};
 for(size_t i=0;i<sizeof bad/sizeof bad[0];i++){Cpu c=setup(32,12);answer=bad[i];invoke(&c,KEYBOARD_CANCELLED);CHECK(ui_calls==1&&!write_calls);}
}
static void huge_capacity(void){size_t cap=1u<<20;long page=sysconf(_SC_PAGESIZE);CHECK(page>0);size_t pages=(cap+(size_t)page-1)/(size_t)page;unsigned char*resident=calloc(pages,1);CHECK(resident);uint8_t*mapped=mmap(NULL,cap,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);CHECK(mapped!=MAP_FAILED);
#ifdef MADV_NOHUGEPAGE
 CHECK(!madvise(mapped,cap,MADV_NOHUGEPAGE));
#endif
 Cpu c=setup(cap,12);string[0]=(uintptr_t)mapped;span(mapped,cap,true,true);answer="Alexandre";invoke(&c,0);CHECK(bytes_written==20&&mapped[0]=='A'&&mapped[1]==0&&mapped[18]==0&&mapped[19]==0);CHECK(!mincore(mapped,cap,resident));for(size_t i=1;i<pages;i++)CHECK(!(resident[i]&1));CHECK(!munmap(mapped,cap));free(resident);
}
int main(void){basic();bounds();utf8_modes();scalar_conversion();huge_capacity();printf("PASS %u actual SDK swkbd UTF8+UTF16/bounds/cancel/reuse/residency assertions\n",checks);}
