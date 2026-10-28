/* SAF access occurs in the fresh game process, never through a launcher-owned FD. */
#include "native_package_android.h"
#include "adapters/title_adapter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <jni.h>
static int uri_fd(const char*uri,char*why,size_t cap){
 JNIEnv*e=(JNIEnv*)SDL_GetAndroidJNIEnv();jobject activity=(jobject)SDL_GetAndroidActivity();
 if(!e||!activity){if(why&&cap)snprintf(why,cap,"Android package access is unavailable in this game process");return -1;}
 int fd=-1;jclass ac=NULL,uc=NULL,rc=NULL,pc=NULL;jobject parsed=NULL,resolver=NULL,pfd=NULL;jstring text=NULL,mode=NULL;
 ac=(*e)->GetObjectClass(e,activity);uc=(*e)->FindClass(e,"android/net/Uri");if(!ac||!uc)goto failed;
 jmethodID get=(*e)->GetMethodID(e,ac,"getContentResolver","()Landroid/content/ContentResolver;"),parse=(*e)->GetStaticMethodID(e,uc,"parse","(Ljava/lang/String;)Landroid/net/Uri;");if(!get||!parse)goto failed;
 text=(*e)->NewStringUTF(e,uri);mode=(*e)->NewStringUTF(e,"r");if(!text||!mode)goto failed;
 parsed=(*e)->CallStaticObjectMethod(e,uc,parse,text);if((*e)->ExceptionCheck(e)||!parsed)goto failed;
 resolver=(*e)->CallObjectMethod(e,activity,get);if((*e)->ExceptionCheck(e)||!resolver)goto failed;
 rc=(*e)->GetObjectClass(e,resolver);if(!rc)goto failed;jmethodID open=(*e)->GetMethodID(e,rc,"openFileDescriptor","(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");if(!open)goto failed;
 pfd=(*e)->CallObjectMethod(e,resolver,open,parsed,mode);if((*e)->ExceptionCheck(e)||!pfd)goto failed;
 pc=(*e)->GetObjectClass(e,pfd);if(!pc)goto failed;jmethodID detach=(*e)->GetMethodID(e,pc,"detachFd","()I");if(!detach)goto failed;
 fd=(*e)->CallIntMethod(e,pfd,detach);if((*e)->ExceptionCheck(e)){fd=-1;goto failed;}goto done;
failed:if((*e)->ExceptionCheck(e))(*e)->ExceptionClear(e);if(why&&cap)snprintf(why,cap,"Cannot reopen selected package; grant file access again and retry");
done:
 if(pfd){if(pc){jmethodID close=(*e)->GetMethodID(e,pc,"close","()V");if(close)(*e)->CallVoidMethod(e,pfd,close);if((*e)->ExceptionCheck(e))(*e)->ExceptionClear(e);}(*e)->DeleteLocalRef(e,pfd);}
 if(mode)(*e)->DeleteLocalRef(e,mode);if(text)(*e)->DeleteLocalRef(e,text);if(parsed)(*e)->DeleteLocalRef(e,parsed);if(resolver)(*e)->DeleteLocalRef(e,resolver);if(pc)(*e)->DeleteLocalRef(e,pc);if(rc)(*e)->DeleteLocalRef(e,rc);if(uc)(*e)->DeleteLocalRef(e,uc);if(ac)(*e)->DeleteLocalRef(e,ac);(*e)->DeleteLocalRef(e,activity);return fd;
}
#endif
#include <unistd.h>
NxRomfs *shared_package_open_romfs(char*why,size_t cap){
 if(why&&cap)why[0]=0;const char*uri=getenv("LARECOMP_PACKAGE_URI"),*keys=getenv("LARECOMP_PACKAGE_KEYS");if(!uri||!*uri)uri=getenv("LARECOMP_PACKAGE_PATH");
 if(!uri||!*uri||!keys||!*keys){if(why&&cap)snprintf(why,cap,"Package source or private keys are missing");return NULL;}
 NxPackage*p=NULL;
 if(!strncmp(uri,"content://",10)){
#ifdef __ANDROID__
  int fd=uri_fd(uri,why,cap);if(fd<0)return NULL;p=nx_package_open_fd(fd,keys,why,cap);close(fd);
#else
  if(why&&cap)snprintf(why,cap,"Android content URI requires an Android game process");return NULL;
#endif
 }else p=nx_package_open(uri,keys,why,cap);
 if(!p)return NULL;
 const TitleAdapter*a=title_adapter_selected();const char*root=getenv("LARECOMP_GAME_ROOT");char exefs[4096];
 int n=root?snprintf(exefs,sizeof exefs,"%s/exefs",root):-1;
 if(!a||nx_package_title_id(p)!=a->program_id||n<0||(size_t)n>=sizeof exefs){
  if(why&&cap)snprintf(why,cap,"Selected package does not match the prepared game");nx_package_close(p);return NULL;
 }
 // Provider preserves and rejects every conflicting existing ExeFS file. Bind the
 // reopened URI's small originals to the generation already hash-verified by the
 // native loader; never silently combine one version's code with another RomFS.
 if(!nx_package_extract_exefs(p,exefs,why,cap)){nx_package_close(p);return NULL;}
 NxRomfs*r=nx_package_open_romfs(p,why,cap);nx_package_close(p);return r;
}
