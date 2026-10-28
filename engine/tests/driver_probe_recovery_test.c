/* Actual fatal/probe helpers, real temporary files, controlled device status. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define __ANDROID__ 1
#define LOG(...) ((void)0)
typedef int VkResult;
#define VK_ERROR_SURFACE_LOST_KHR (-1000000000)
#define VK_ERROR_OUT_OF_DATE_KHR (-1000001004)
#define VK_SUBOPTIMAL_KHR 1000001003
static atomic_bool g_surface_stale,ui_ready;
static bool g_sc_turned,custom_driver_on;
static char driver_probe[512],g_line[512];
static uint64_t g_pc;
static pthread_mutex_t driver_probe_lock=PTHREAD_MUTEX_INITIALIZER;
#include "driver_probe_production.inc"
static const char*known="nvn: recording cannot chain a command chunk to itself\n";
static bool exists(const char*path){struct stat st;return !lstat(path,&st);}
int main(int argc,char**argv){
 assert(argc==3);int n=snprintf(driver_probe,sizeof driver_probe,"%s/.probe",argv[1]);assert(n>0&&(size_t)n<sizeof driver_probe);char path[512];strcpy(path,driver_probe);
 bool directory=!strcmp(argv[2],"unlink-failure");if(directory)assert(!mkdir(path,0700));else {FILE*f=fopen(path,"wb");assert(f&&!fclose(f));}
 custom_driver_on=true;atomic_store(&ui_ready,true);bool cleared=false;
 if(!strcmp(argv[2],"known")){rt_fatal_msg("%s",known);cleared=true;assert(strstr(g_line,"cannot chain"));}
 else if(!strcmp(argv[2],"signal")){rt_crash_fault("signal 11",16,0x1234);rt_fatal_msg("%s",known);assert(g_pc==0x1234&&strstr(g_line,"signal 11"));}
 else if(!strcmp(argv[2],"unknown")){rt_fatal_msg("unknown runtime failure\n");rt_fatal_msg("%s",known);}
 else if(!strcmp(argv[2],"gpu")){rt_fatal_msg("vkQueueSubmit failed: VK_ERROR_DEVICE_LOST\n");}
 else if(!strcmp(argv[2],"init")){atomic_store(&ui_ready,false);rt_fatal_msg("%s",known);}
 else if(!strcmp(argv[2],"system")){custom_driver_on=false;rt_fatal_msg("%s",known);}
 else if(!strcmp(argv[2],"similar")){rt_fatal_msg("%sextra unrelated diagnostic",known);}
 else if(!strcmp(argv[2],"missing")){assert(!unlink(path));rt_fatal_msg("%s",known);cleared=true;}
 else if(!strcmp(argv[2],"unlink-failure")){rt_fatal_msg("%s",known);assert(driver_probe[0]);}
 else if(!strcmp(argv[2],"present")){
  for(unsigned i=0;i<59;i++){assert(!vk_present_result(0));assert(exists(path));}
  assert(!vk_present_result(VK_ERROR_SURFACE_LOST_KHR)&&atomic_load(&g_surface_stale)&&exists(path));
  assert(!vk_present_result(0));cleared=true;
  for(unsigned i=0;i<100;i++)assert(!vk_present_result(0));
 } else assert(false);
 assert(exists(path)==!cleared);if(cleared)assert(!driver_probe[0]);else assert(driver_probe[0]);
 if(!cleared){if(directory)assert(!rmdir(path));else assert(!unlink(path));}
 printf("%s: marker %s\n",argv[2],cleared?"cleared":"retained");
}
