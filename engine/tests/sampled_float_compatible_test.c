/* Actual production numeric-domain classifier, no Vulkan allocation or assets. */
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "production_types.inc"
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static VkFormat depth24_stencil8(void){return VK_FORMAT_D32_SFLOAT_S8_UINT;}
#include "production_helpers.inc"
static bool integer_guest(unsigned f){switch(f){case 3:case 4:case 8:case 9:case 0xb:case 0xc:case 0xf:case 0x10:case 0x14:case 0x15:case 0x17:case 0x18:case 0x27:case 0x28:case 0x2c:case 0x2d:case 0x2f:case 0x30:case 0x31:case 0x3e:return true;default:return false;}}
static bool known_guest(unsigned f){return(f>=1&&f<=0x19)||(f>=0x25&&f<=0x50)||f==0x6d||f==0x71||f==0x76||(f>=0x79&&f<=0x9b);}
int main(void){
 CHECK(!vk_sampled_float_compatible(NULL,NULL,false));
 for(unsigned f=0;f<1024;f++){
  NvnTexture t={.format=f};bool expected=known_guest(f)&&!integer_guest(f);
  CHECK(vk_sampled_float_compatible(&t,NULL,false)==expected);
  TexBackend b={.format=fmt_of(f).vf,.aspect=renderer_format_is_depth(f)?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT};t.backend=&b;
  CHECK(vk_sampled_float_compatible(&t,NULL,false)==expected);
 }
 // View reinterpretations use effective actual format, not base format alone.
 NvnTexture t={.format=0x25};NvnTextureView v={.setmask=4,.format=0x27};
 CHECK(!vk_sampled_float_compatible(&t,&v,true));CHECK(vk_sampled_float_compatible(&t,&v,false));
 TexBackend b={.format=VK_FORMAT_R8G8B8A8_UNORM,.aspect=VK_IMAGE_ASPECT_COLOR_BIT};t.backend=&b;
 CHECK(!vk_sampled_float_compatible(&t,&v,true));
 b.fmt_listed=true;b.fmt_twin=VK_FORMAT_R8G8B8A8_SRGB;CHECK(vk_sampled_float_compatible(&t,&v,true));
 b.fmt_twin=VK_FORMAT_R8G8B8A8_UINT;CHECK(!vk_sampled_float_compatible(&t,&v,true));
 t.format=0x27;b.format=VK_FORMAT_R8G8B8A8_UINT;v.format=0x25;b.fmt_twin=VK_FORMAT_R8G8B8A8_UNORM;
 CHECK(vk_sampled_float_compatible(&t,&v,true));CHECK(!vk_sampled_float_compatible(&t,NULL,false));
 // Incompatible byte classes preserve the existing integer backing format.
 v.format=1;CHECK(!vk_sampled_float_compatible(&t,&v,true));
 // A converted fallback backend keeps its own float format, not the requested view.
 t.format=0x49;b.format=VK_FORMAT_R8G8B8A8_UNORM;b.fmt_listed=false;v.format=0x27;
 CHECK(vk_sampled_float_compatible(&t,&v,true));
 b.format=VK_FORMAT_UNDEFINED;CHECK(!vk_sampled_float_compatible(&t,NULL,false));
 b.format=(VkFormat)0x7fffffff;CHECK(!vk_sampled_float_compatible(&t,NULL,false));
 t.format=0x25;b.format=VK_FORMAT_R8G8B8A8_UNORM;v.format=0x20;CHECK(!vk_sampled_float_compatible(&t,&v,true));
 for(unsigned f=0x32;f<=0x36;f++){
  t=(NvnTexture){.format=f};v=(NvnTextureView){.setmask=16,.ds_mode=0};
  CHECK(vk_sampled_float_compatible(&t,&v,true));v.ds_mode=1;CHECK(!vk_sampled_float_compatible(&t,&v,true));
  v.ds_mode=2;CHECK(!vk_sampled_float_compatible(&t,&v,true));
  t.ds_mode=1;v.ds_mode=0;CHECK(vk_sampled_float_compatible(&t,&v,true));CHECK(!vk_sampled_float_compatible(&t,&v,false));
 }
 // Every integer Vulkan class mapped by fmt_of is rejected even for a nominal
 // float guest texture with a backend fallback; unknown actual formats too.
 for(unsigned f=1;f<=0x9b;f++)if(integer_guest(f)){
  t=(NvnTexture){.format=0x25,.backend=&b};b=(TexBackend){.format=fmt_of(f).vf,.aspect=VK_IMAGE_ASPECT_COLOR_BIT};CHECK(!vk_sampled_float_compatible(&t,NULL,false));
 }
 printf("%u actual sampled-float compatibility checks passed\n",checks);return 0;
}
