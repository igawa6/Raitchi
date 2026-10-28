/* Standalone launcher JNI: no SDL, renderer, title selection or guest execution. */
#include "nxpkg.h"
#include "runtime/native_prepare.h"
#include "shared_contract.h"
#include "adapters/title_adapter.h"
#include "adapters/generic/generic.h"
#include "native_package_metadata.h"
#include <jni.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
extern bool shared_sha256_bytes(const void*,size_t,uint8_t[32]);
static void io_error(JNIEnv*env,const char*why){if((*env)->ExceptionCheck(env))return;jclass cls=(*env)->FindClass(env,"java/io/IOException");if(cls){(*env)->ThrowNew(env,cls,why);(*env)->DeleteLocalRef(env,cls);}}
static bool directory(const char*path){if(mkdir(path,0700)&&errno!=EEXIST)return false;struct stat st;return !lstat(path,&st)&&S_ISDIR(st.st_mode);}
static char *quoted(const char*value){return native_package_json_quote(value);}
static bool write_icon(const char*root,uint64_t id,const uint8_t*data,size_t size,char*path,size_t cap){if(!data||!size)return true;if(!directory(root))return false;int n=snprintf(path,cap,"%s/%016llx.dat",root,(unsigned long long)id);if(n<0||(size_t)n>=cap)return false;char temp[4096];n=snprintf(temp,sizeof temp,"%s/.icon-XXXXXX",root);if(n<0||(size_t)n>=sizeof temp)return false;int fd=mkstemp(temp);if(fd<0)return false;size_t done=0;while(done<size){ssize_t w=write(fd,data+done,size-done);if(w<0&&errno==EINTR)continue;if(w<=0){close(fd);unlink(temp);return false;}done+=(size_t)w;}bool ok=!fsync(fd);if(close(fd))ok=false;if(ok&&rename(temp,path))ok=false;if(!ok)unlink(temp);return ok;}
JNIEXPORT jbyteArray JNICALL Java_com_lasrecomp_android_RaitchiMetadata_inspectNative(JNIEnv*env,jclass cls,jint fd,jstring jkeys,jstring jicons){
 (void)cls;const char*keys=NULL,*icons=NULL;NxPackage*p=NULL;jbyteArray answer=NULL;char why[256]="Cannot inspect package metadata",icon_path[4096]="";NxPackageMetadata m={0};char*json=NULL;
 if(fd<0||!jkeys||!jicons){io_error(env,"A readable package, keys file and private icon cache are required");return NULL;}
 keys=(*env)->GetStringUTFChars(env,jkeys,NULL);if(!keys)goto done;icons=(*env)->GetStringUTFChars(env,jicons,NULL);if(!icons)goto done;
 p=nx_package_open_fd(fd,keys,why,sizeof why);if(!p)goto failed;
 if(!nx_package_read_metadata(p,&m,why,sizeof why))goto failed;
 uint64_t id=nx_package_title_id(p);NativePackageClass c;native_package_classify(p,&m,&c);
 if(!write_icon(icons,id,m.icon,m.icon_size,icon_path,sizeof icon_path)){snprintf(why,sizeof why,"Cannot publish game icon cache file");goto failed;}
 json=native_package_metadata_json(id,&m,icon_path,&c);if(!json){snprintf(why,sizeof why,"Package metadata allocation failed");goto failed;}
 size_t json_size=strlen(json);if(json_size>INT32_MAX){snprintf(why,sizeof why,"Package metadata is too large");goto failed;}answer=(*env)->NewByteArray(env,(jsize)json_size);if(answer)(*env)->SetByteArrayRegion(env,answer,0,(jsize)json_size,(const jbyte*)json);goto done;
failed:io_error(env,why);
done:free(json);nx_package_metadata_clear(&m);nx_package_close(p);if(keys)(*env)->ReleaseStringUTFChars(env,jkeys,keys);if(icons)(*env)->ReleaseStringUTFChars(env,jicons,icons);return answer;
}
JNIEXPORT jstring JNICALL Java_com_lasrecomp_android_NativePackage_prepare(JNIEnv*env,jclass cls,jint fd,jstring jkeys,jstring jroot,jstring jtitle){
 (void)cls;const char*keys=NULL,*root=NULL,*title=NULL;NxPackage*package=NULL;jstring answer=NULL;char why[256]="Native package preparation failed";char*quote=NULL,*json=NULL;
 if(fd<0||!jkeys||!jroot||!jtitle){io_error(env,"A seekable package, private keys, output directory and supported title are required");return NULL;}
 keys=(*env)->GetStringUTFChars(env,jkeys,NULL);if(!keys)goto done;
 root=(*env)->GetStringUTFChars(env,jroot,NULL);if(!root)goto done;
 title=(*env)->GetStringUTFChars(env,jtitle,NULL);if(!title)goto done;
 bool generic=!strcmp(title,TITLE_GENERIC_KEY);char version[TITLE_GENERIC_VERSION_MAX+1]="";
 uint64_t program=native_prepare_program_id(title);if((!program&&!generic)||!*keys||!*root){snprintf(why,sizeof why,"Unsupported title or missing native package paths");goto failed;}
 package=nx_package_open_fd(fd,keys,why,sizeof why);if(!package)goto failed;
 if(generic){
  // Generic startup only for a package that inspection reports as tryable.
  NxPackageMetadata m={0};if(!nx_package_read_metadata(package,&m,why,sizeof why))goto failed;
  NativePackageClass c;native_package_classify(package,&m,&c);title_generic_version_token(m.version,version);nx_package_metadata_clear(&m);
  if(!c.tryable){snprintf(why,sizeof why,"%s",c.reason);goto failed;}
  program=nx_package_title_id(package);
 }
 if(nx_package_title_id(package)!=program){snprintf(why,sizeof why,"Package title does not match selected game");goto failed;}
 char exefs[4096],manifest[4096];int n=snprintf(exefs,sizeof exefs,"%s/exefs",root),m=snprintf(manifest,sizeof manifest,"%s/native.manifest",root);
 if(n<0||(size_t)n>=sizeof exefs||m<0||(size_t)m>=sizeof manifest){snprintf(why,sizeof why,"Native package output path is too long");goto failed;}
 if(!directory(root)||!directory(exefs)){snprintf(why,sizeof why,"Cannot create private native package preparation directory");goto failed;}
 if(!nx_package_extract_exefs(package,exefs,why,sizeof why))goto failed;
 uint8_t contract[32];const char*hex=SHARED_NATIVE_CONTRACT_SHA256_HEX;
 for(unsigned i=0;i<32;i++){unsigned v;if(sscanf(hex+2*i,"%2x",&v)!=1){snprintf(why,sizeof why,"Native runtime contract is unavailable");goto failed;}contract[i]=(uint8_t)v;}
 NativePrepareResult result;if(!native_prepare_exefs(title,exefs,manifest,contract,shared_sha256_bytes,&result,why,sizeof why))goto failed;
 if(result.program_id!=program){snprintf(why,sizeof why,"Prepared program ID does not match the package");goto failed;}
 quote=quoted(manifest);if(!quote){snprintf(why,sizeof why,"Native package result allocation failed");goto failed;}
 size_t size=strlen(quote)+192;json=malloc(size);if(!json){snprintf(why,sizeof why,"Native package result allocation failed");goto failed;}
 if(generic)snprintf(json,size,"{\"title\":\"%s\",\"modules\":%u,\"manifest\":%s,\"programId\":\"%016llx\",\"version\":\"%s\"}",result.title,result.modules,quote,(unsigned long long)result.program_id,version);
 else snprintf(json,size,"{\"title\":\"%s\",\"modules\":%u,\"manifest\":%s}",result.title,result.modules,quote);
 answer=(*env)->NewStringUTF(env,json);goto done;
failed:io_error(env,why);
done:free(quote);free(json);nx_package_close(package);if(keys)(*env)->ReleaseStringUTFChars(env,jkeys,keys);if(root)(*env)->ReleaseStringUTFChars(env,jroot,root);if(title)(*env)->ReleaseStringUTFChars(env,jtitle,title);return answer;
}
