/* Actual diagnostic formatter and format tables; no vertex bytes or GPU calls. */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "shader.h"
#include "production_attributes.inc"
static unsigned checks;
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"line%d %s\n",__LINE__,#x);abort();}}while(0)
static void read_file(FILE*f,char*buf,size_t cap){CHECK(!fflush(f));CHECK(!fseek(f,0,SEEK_SET));size_t n=fread(buf,1,cap-1,f);CHECK(!ferror(f));buf[n]=0;}
int main(void){
 long page=sysconf(_SC_PAGESIZE);unsigned char*p=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);CHECK(p!=MAP_FAILED);CHECK(!mprotect(p+page,page,PROT_NONE));uint32_t*a=(void*)(p+page-16*sizeof(uint32_t));memset(a,0,16*sizeof(uint32_t));
 a[0]=0x27|(2u<<8)|(12u<<13);a[1]=0x51|(3u<<8)|(8u<<13);a[2]=0x52|(31u<<8);ShaderKey key={0};key.attr_kind[0]=2;
 ShaderInfo shader={.attr_mask=7};VtxLayout layout={.attr_mask=7};layout.attr_fmt[0]=VK_FORMAT_R8G8B8A8_UINT;layout.attr_fmt[1]=VK_FORMAT_R8_USCALED;layout.attr_fmt[2]=VK_FORMAT_R32G32B32A32_SFLOAT;layout.attr_bind[0]=2;layout.attr_bind[1]=3;layout.attr_bind[2]=16;layout.attr_off[0]=12;layout.attr_off[1]=8;layout.stride[2]=32;layout.stride[3]=4;layout.stride[16]=16;layout.divisor[3]=1;
 uint32_t streams[16][2]={{0}};streams[2][0]=32;streams[3][0]=4;streams[3][1]=1;Range ranges[17]={{0}};ranges[2].size=1000;ranges[3].size=200;ranges[16].size=16;
 char out[16384],needle[100];FILE*f=tmpfile();CHECK(f);material_attribute_metadata(f,&shader,&key,&layout,a,3,streams,4,ranges);read_file(f,out,sizeof out);CHECK(strstr(out,"required_mask=00000007 supplied_count=3"));CHECK(strstr(out,"nvn_format=27 source_stream=2 relative_offset=12 derived_kind=2 shader_kind=2"));CHECK(strstr(out,"binding=2 binding_offset=12 stride=32 divisor=0 source_stride=32 source_divisor=0 bound_bytes=1000"));snprintf(needle,sizeof needle,"selected_vk_format=%u",VK_FORMAT_R8_USCALED);CHECK(strstr(out,needle));CHECK(strstr(out,"nvn_format=52 source_stream=31"));CHECK(strstr(out,"binding=16 binding_offset=0 stride=16 divisor=0 source_stride=0"));CHECK(!fclose(f));
 // Request input count cannot make the formatter cross16 real attribute words.
 shader.attr_mask=~0u;f=tmpfile();CHECK(f);material_attribute_metadata(f,&shader,&key,&layout,a,100,streams,100,ranges);read_file(f,out,sizeof out);unsigned lines=0;for(char*q=out;(q=strstr(q," attribute slot="));q++)lines++;CHECK(lines==16);CHECK(strstr(out,"unlogged_required_mask=ffff0000"));CHECK(!fclose(f));
 // Unprovided attributes with no low16 shader inputs read no array/key/layout.
 shader.attr_mask=1u<<31;f=tmpfile();CHECK(f);material_attribute_metadata(f,&shader,(void*)1,(void*)1,(void*)(p+page),0,(void*)1,0,(void*)1);read_file(f,out,sizeof out);CHECK(!strstr(out," attribute slot="));CHECK(!fclose(f));
 CHECK(!munmap(p,page*2));printf("%u actual attribute-metadata assertions passed\n",checks);return 0;}
