/* Asset-free offscreen Vulkan arithmetic probe. No renderer/runtime exports.
 * Built with tools/check_shared_gpu_addresses.py; invoke once in a fresh helper
 * process under the app UID. The caller supplies its installed hook directory.
 */
#include <volk.h>
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_address_shaders.h"
#define N 512u
#define BASE UINT64_C(0x1240000000)
#define SIZE UINT64_C(0xd0000000)
#define TRY(call) do { VkResult r=(call); if(r!=VK_SUCCESS){snprintf(report,cap,"{\"error\":\"%s\",\"VkResult\":%d}",#call,r);goto cleanup;} }while(0)

__attribute__((visibility("default")))
int shared_gpu_addresses_probe(const char *driver_dir,const char *driver_lib,
                              const char *hooks_dir,const char *tmp_dir,char *report,size_t cap) {
    VkInstance instance=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;
    VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;
    VkDescriptorPool descriptors=VK_NULL_HANDLE;VkCommandPool pool=VK_NULL_HANDLE;
    VkPipeline pipelines[3]={0};VkShaderModule shaders[3]={0};VkFence fence=VK_NULL_HANDLE;
    uint32_t *mapped=NULL;int result=1;void *driver=NULL;
    if(!report||cap<512)return 2;
    snprintf(report,cap,"{\"error\":\"initialization incomplete\"}");
    if(driver_dir&&*driver_dir) {
        if(!driver_lib||!hooks_dir){snprintf(report,cap,"{\"error\":\"custom driver requires library/hooks\"}");return 2;}
        driver=adrenotools_open_libvulkan(RTLD_NOW|RTLD_LOCAL,ADRENOTOOLS_DRIVER_CUSTOM,tmp_dir,hooks_dir,driver_dir,driver_lib,NULL,NULL);
        PFN_vkGetInstanceProcAddr gipa=driver?(PFN_vkGetInstanceProcAddr)dlsym(driver,"vkGetInstanceProcAddr"):NULL;
        if(!gipa){snprintf(report,cap,"{\"error\":\"explicit custom driver failed\"}");return 2;}
        volkInitializeCustom(gipa);
    } else if(volkInitialize()!=VK_SUCCESS){snprintf(report,cap,"{\"error\":\"stock Vulkan loader failed\"}");return 2;}
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="public-address-probe",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    TRY(vkCreateInstance(&ici,NULL,&instance));volkLoadInstance(instance);
    uint32_t count=0;TRY(vkEnumeratePhysicalDevices(instance,&count,NULL));
    if(!count){snprintf(report,cap,"{\"error\":\"no Vulkan devices\"}");goto cleanup;}
    VkPhysicalDevice physical;count=1;VkResult enumerate=vkEnumeratePhysicalDevices(instance,&count,&physical);
    if(enumerate!=VK_SUCCESS&&enumerate!=VK_INCOMPLETE)goto cleanup;
    VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);
    if(driver&&strstr(props.deviceName,"Turnip")==NULL){snprintf(report,cap,"{\"error\":\"custom request did not select Turnip\"}");goto cleanup;}
    VkPhysicalDeviceFeatures have;vkGetPhysicalDeviceFeatures(physical,&have);
    uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,NULL);
    VkQueueFamilyProperties *families=calloc(nq,sizeof(*families));
    if(!families)goto cleanup;vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,families);
    uint32_t family=0;while(family<nq&&!(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;
    free(families);if(family==nq)goto cleanup;
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceFeatures enabled={.shaderInt64=have.shaderInt64};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.pEnabledFeatures=&enabled};
    TRY(vkCreateDevice(physical,&di,NULL,&device));volkLoadDevice(device);
    VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=N*32,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(device,&bi,NULL,&buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);
    VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    uint32_t type=UINT32_MAX;for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){
        type=i;if(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)break;
    }
    if(type==UINT32_MAX)goto cleanup;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    TRY(vkAllocateMemory(device,&ai,NULL,&memory));TRY(vkBindBufferMemory(device,buffer,memory,0));
    TRY(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,(void**)&mapped));
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo sci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    TRY(vkCreateDescriptorSetLayout(device,&sci,NULL,&set_layout));
    VkPushConstantRange pc={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=8};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&pc};
    TRY(vkCreatePipelineLayout(device,&li,NULL,&layout));
    const uint32_t *words[3]={address_fixed,address_dynamic,address_int64};
    size_t sizes[3]={sizeof(address_fixed),sizeof(address_dynamic),sizeof(address_int64)};
    for(unsigned i=0;i<(have.shaderInt64?3u:2u);i++){
        VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizes[i],.pCode=words[i]};
        TRY(vkCreateShaderModule(device,&si,NULL,&shaders[i]));
        VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,
            .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shaders[i],.pName="main"}};
        TRY(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipelines[i]));
    }
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    TRY(vkCreateDescriptorPool(device,&dpi,NULL,&descriptors));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptors,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;TRY(vkAllocateDescriptorSets(device,&dai,&set));
    VkDescriptorBufferInfo dbi={buffer,0,N*32};VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&dbi};
    vkUpdateDescriptorSets(device,1,&write,0,NULL);
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};TRY(vkCreateCommandPool(device,&cpi,NULL,&pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;TRY(vkAllocateCommandBuffers(device,&cai,&command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};TRY(vkCreateFence(device,&fi,NULL,&fence));
    unsigned old_mismatch[2]={0},lex_mismatch[2]={0},int64_mismatch[2]={0},offset_mismatch[2]={0};
    uint64_t edges[]={0,UINT64_MAX,BASE-1,BASE,BASE+1,BASE+SIZE-1,BASE+SIZE,UINT64_C(0x12ffffffff),UINT64_C(0x1300000000),UINT64_C(0x1300000001),BASE+0x80000000};
    for(unsigned mode=0;mode<2;mode++){
        for(uint32_t i=0;i<N;i++){
            uint64_t address=i<sizeof(edges)/sizeof(edges[0])?edges[i]:BASE+((uint64_t)(i*2654435761u)%SIZE);
            int32_t off=mode?(i%3==0?-1:i%3==1?1:0):0;
            mapped[i*4]=(uint32_t)address;mapped[i*4+1]=(uint32_t)(address>>32);mapped[i*4+2]=(uint32_t)off;mapped[i*4+3]=0;
        }
        memset(mapped+N*4,0xa5,N*16);
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.size=VK_WHOLE_SIZE};TRY(vkFlushMappedMemoryRanges(device,1,&range));
        TRY(vkResetCommandPool(device,pool,0));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        TRY(vkBeginCommandBuffer(command,&begin));uint32_t push[]={N,mode};
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,push);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[mode]);vkCmdDispatch(command,(N+63)/64,1,1);
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT};
        if(have.shaderInt64){vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[2]);vkCmdDispatch(command,(N+63)/64,1,1);}
        barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
        TRY(vkEndCommandBuffer(command));TRY(vkResetFences(device,1,&fence));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
        TRY(vkQueueSubmit(queue,1,&submit,fence));TRY(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(10000000000)));TRY(vkInvalidateMappedMemoryRanges(device,1,&range));
        for(uint32_t i=0;i<N;i++){
            uint64_t a=((uint64_t)mapped[i*4+1]<<32)|mapped[i*4];uint64_t delta=a+(uint64_t)(int64_t)(int32_t)mapped[i*4+2]-BASE;
            uint32_t ok=delta<SIZE;uint32_t*r=mapped+(N+i)*4;
            old_mismatch[mode]+=r[0]!=ok;lex_mismatch[mode]+=r[1]!=ok;offset_mismatch[mode]+=r[2]!=(uint32_t)delta;
            if(have.shaderInt64)int64_mismatch[mode]+=r[3]!=ok;
        }
    }
    snprintf(report,cap,"{\"gpu\":\"%s\",\"shaderInt64\":%u,\"cases_per_mode\":%u,\"old_mismatch\":[%u,%u],\"lex_mismatch\":[%u,%u],\"offset_mismatch\":[%u,%u],\"int64_mismatch\":[%u,%u]}",props.deviceName,have.shaderInt64,N,old_mismatch[0],old_mismatch[1],lex_mismatch[0],lex_mismatch[1],offset_mismatch[0],offset_mismatch[1],int64_mismatch[0],int64_mismatch[1]);
    result=(old_mismatch[0]+old_mismatch[1]+lex_mismatch[0]+lex_mismatch[1]+offset_mismatch[0]+offset_mismatch[1]+int64_mismatch[0]+int64_mismatch[1])?3:0;
cleanup:
    if(device){vkDeviceWaitIdle(device);if(mapped)vkUnmapMemory(device,memory);if(fence)vkDestroyFence(device,fence,NULL);if(pool)vkDestroyCommandPool(device,pool,NULL);if(descriptors)vkDestroyDescriptorPool(device,descriptors,NULL);
        for(unsigned i=0;i<3;i++){if(pipelines[i])vkDestroyPipeline(device,pipelines[i],NULL);if(shaders[i])vkDestroyShaderModule(device,shaders[i],NULL);}if(layout)vkDestroyPipelineLayout(device,layout,NULL);if(set_layout)vkDestroyDescriptorSetLayout(device,set_layout,NULL);if(buffer)vkDestroyBuffer(device,buffer,NULL);if(memory)vkFreeMemory(device,memory,NULL);vkDestroyDevice(device,NULL);}
    if(instance)vkDestroyInstance(instance,NULL);
    /* Keep custom-loader hooks alive until this fresh helper process exits. */
    return result;
}
