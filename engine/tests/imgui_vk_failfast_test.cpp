/* Actual ImGui VkResult callback; a thrown test fatal cannot fall through. */
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
using VkResult=int;
static constexpr VkResult VK_SUCCESS=0;
static unsigned checks,fatals;
static char reason[256];
#define CHECK(x) do{checks++;if(!(x)){std::fprintf(stderr,"line%d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static void fail(const char *format,...){va_list args;va_start(args,format);std::vsnprintf(reason,sizeof reason,format,args);va_end(args);fatals++;throw 1;}
#define FATAL(...) fail(__VA_ARGS__)
#include "imgui_vk_failfast_fragment.h"
int main(){
 imgui_vk_result(VK_SUCCESS);CHECK(!fatals);
 // Vulkan out-of-host/device memory, map failure, lost device and unexpected
 // non-success statuses must stop before the caller can copy mapped memory.
 for(int result:{-1,-2,-4,-5,-6,-7,-8,-9,-10,-11,-12,-13,1}){
  unsigned before=fatals;bool reached_copy=false;
  try{imgui_vk_result(result);reached_copy=true;}catch(int value){CHECK(value==1);}
  CHECK(!reached_copy&&fatals==before+1);char expected[32];std::snprintf(expected,sizeof expected,"VkResult=%d",result);CHECK(std::strstr(reason,expected));
 }
 imgui_vk_result(VK_SUCCESS);CHECK(fatals==13);std::printf("{\"state\":\"passed\",\"checks\":%u,\"failed_allocation_map_stops_before_copy\":true}\n",checks);return 0;
}
