/* Actual production feedback view selector with controlled Vulkan view creation. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn/maxwell/shader.h"
typedef uint64_t VkImageView;
typedef unsigned VkImageAspectFlags;
typedef struct {unsigned r,g,b,a;}VkComponentMapping;
#define VK_NULL_HANDLE 0
#define VK_IMAGE_VIEW_TYPE_2D 1
#define VK_IMAGE_VIEW_TYPE_2D_ARRAY 5
#define VK_IMAGE_ASPECT_COLOR_BIT 1
#define VK_IMAGE_ASPECT_DEPTH_BIT 2
#define VK_IMAGE_ASPECT_STENCIL_BIT 4
typedef struct{uint64_t image;int base_type,format;unsigned samples,width,height,depth,layers,levels,aspect;}TexBackend;
static VkImageView feedback_view[4];static TexBackend*feedback_image[4];static int nfeedback;
static unsigned calls,checks;static bool fail;
static struct{TexBackend*b;int type,format;unsigned lv,nlv,ly,nly,aspect;VkComponentMapping sw;}last;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static VkImageView vk_image_view(TexBackend*b,int type,int fmt,unsigned lv,unsigned nlv,unsigned ly,unsigned nly,VkComponentMapping sw,unsigned aspect){
 calls++;last.b=b;last.type=type;last.format=fmt;last.lv=lv;last.nlv=nlv;last.ly=ly;last.nly=nly;last.sw=sw;last.aspect=aspect;return fail?0:0xbeef;
}
#include "feedback_view.production.h"
int main(void){
 TexBackend b={.image=0x123456,.base_type=VK_IMAGE_VIEW_TYPE_2D,.format=130,.samples=1,.width=1024,.height=1024,.depth=1,.layers=1,.levels=1,.aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT};TexBackend before=b;
 nfeedback=1;feedback_view[0]=0xabcd;feedback_image[0]=&b;
 CHECK(feedback_sample_view(0,TEXTYPE_2D,false)==0xabcd&&calls==0);CHECK(feedback_sample_view(0,TEXTYPE_2D_ARRAY,false)==0xbeef&&calls==1);
 CHECK(last.b==&b&&last.b->image==0x123456);CHECK(last.type==VK_IMAGE_VIEW_TYPE_2D_ARRAY&&last.format==130);
 CHECK(last.lv==0&&last.nlv==1&&last.ly==0&&last.nly==1);CHECK(last.aspect==VK_IMAGE_ASPECT_DEPTH_BIT);
 CHECK(!last.sw.r&&!last.sw.g&&!last.sw.b&&!last.sw.a);CHECK(!memcmp(&b,&before,sizeof b));
 for(unsigned i=0;i<100;i++)CHECK(feedback_sample_view(0,TEXTYPE_2D_ARRAY,false)==0xbeef);
 b.aspect=VK_IMAGE_ASPECT_COLOR_BIT;b.format=37;CHECK(feedback_sample_view(0,TEXTYPE_2D_ARRAY,false)==0xbeef);CHECK(last.aspect==VK_IMAGE_ASPECT_COLOR_BIT&&last.format==37);b=before;
 // Do not fabricate a single-sample or multi-subresource contract for other snapshots.
 unsigned old=calls;
 b.samples=4;CHECK(!feedback_sample_view(0,TEXTYPE_2D_ARRAY,false));b=before;
 b.layers=2;CHECK(!feedback_sample_view(0,TEXTYPE_2D_ARRAY,false));b=before;
 b.levels=2;CHECK(!feedback_sample_view(0,TEXTYPE_2D_ARRAY,false));b=before;
 b.depth=2;CHECK(!feedback_sample_view(0,TEXTYPE_2D_ARRAY,false));b=before;
 b.base_type=VK_IMAGE_VIEW_TYPE_2D_ARRAY;CHECK(!feedback_sample_view(0,TEXTYPE_2D_ARRAY,false));b=before;
 b.image=0;CHECK(!feedback_sample_view(0,TEXTYPE_2D_ARRAY,false));b=before;
 feedback_image[0]=NULL;CHECK(!feedback_sample_view(0,TEXTYPE_2D_ARRAY,false));feedback_image[0]=&b;
 CHECK(!feedback_sample_view(-1,TEXTYPE_2D_ARRAY,false));CHECK(!feedback_sample_view(1,TEXTYPE_2D_ARRAY,false));CHECK(!feedback_sample_view(0,TEXTYPE_CUBE,false));CHECK(calls==old);
 fail=true;CHECK(!feedback_sample_view(0,TEXTYPE_2D_ARRAY,false));CHECK(calls==old+1);CHECK(feedback_sample_view(0,TEXTYPE_2D,false)==0xabcd);CHECK(!memcmp(&b,&before,sizeof b));
 fail=false;
 CHECK(feedback_sample_view(0,TEXTYPE_2D,true)==0xbeef);
 CHECK(last.b==&b&&last.type==VK_IMAGE_VIEW_TYPE_2D&&last.aspect==VK_IMAGE_ASPECT_STENCIL_BIT);
 CHECK(feedback_sample_view(0,TEXTYPE_2D_ARRAY,true)==0xbeef);
 CHECK(last.b==&b&&last.type==VK_IMAGE_VIEW_TYPE_2D_ARRAY&&last.aspect==VK_IMAGE_ASPECT_STENCIL_BIT);
 b.aspect=VK_IMAGE_ASPECT_DEPTH_BIT;old=calls;
 CHECK(!feedback_sample_view(0,TEXTYPE_2D,true));CHECK(!feedback_sample_view(0,TEXTYPE_2D_ARRAY,true));CHECK(calls==old);b=before;
 CHECK(!memcmp(&b,&before,sizeof b));
 printf("%u actual feedback view assertions passed\n",checks);return 0;
}
