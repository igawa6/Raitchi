#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ipc.h"
#define LOG(...) ((void)0)
static uint32_t new_event_handle(void){return 123;}
#include "production_audio_device.inc"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort();}}while(0)
static void list(uint32_t command,int autobuf,size_t size){
 unsigned char bytes[0x310];memset(bytes,0xa5,sizeof bytes);IpcReq request={.cmd=command};IpcResp response={0};
 if(autobuf){request.nc=1;request.c[0]=(IpcBuf){(uintptr_t)(bytes+8),size};}else{request.nb=1;request.b[0]=(IpcBuf){(uintptr_t)(bytes+8),size};}
 audio_device(NULL,&request,&response);unsigned count=size>=0x100;
 CHECK(!response.result&&response.data_size==4);CHECK(*(uint32_t*)response.data==count);
 if(count){CHECK(!strcmp((char*)bytes+8,"AudioTvOutput"));for(unsigned i=8+sizeof("AudioTvOutput");i<0x108;i++)CHECK(!bytes[i]);}
 for(unsigned i=0;i<8;i++)CHECK(bytes[i]==0xa5);
 for(unsigned i=count?0x108:8;i<sizeof bytes;i++)CHECK(bytes[i]==0xa5);
}
int main(void){
 for(unsigned command=0;command<=14;command++)if(command==0||command==6||command==14)
  for(int autobuf=0;autobuf<2;autobuf++)for(unsigned n=0;n<5;n++)list(command,autobuf,(size_t[]){0,1,255,256,512}[n]);
 for(unsigned command=3;command<=13;command++)if(command==3||command==10||command==13)for(unsigned n=0;n<4;n++){
  unsigned char bytes[0x110];memset(bytes,0xa5,sizeof bytes);IpcReq request={.cmd=command,.nb=1};IpcResp response={0};size_t size=(size_t[]){0,255,256,257}[n];request.b[0]=(IpcBuf){(uintptr_t)(bytes+8),size};audio_device(NULL,&request,&response);
  CHECK(response.result==(size<256?0x899u:0));CHECK(!response.data_size);
  if(size>=256){CHECK(!strcmp((char*)bytes+8,"AudioTvOutput"));for(unsigned i=8+sizeof("AudioTvOutput");i<264;i++)CHECK(!bytes[i]);}
  for(unsigned i=0;i<8;i++)CHECK(bytes[i]==0xa5);for(unsigned i=size>=256?264:8;i<sizeof bytes;i++)CHECK(bytes[i]==0xa5);
 }
 IpcReq absent={.cmd=14};IpcResp r={0};audio_device(NULL,&absent,&r);CHECK(!r.result&&r.data_size==4&&*(uint32_t*)r.data==0);absent.cmd=13;memset(&r,0,sizeof r);audio_device(NULL,&absent,&r);CHECK(r.result==0x899);
 printf("%u actual audio device output enumeration/name assertions passed; controlled event helper\n",checks);return 0;
}
