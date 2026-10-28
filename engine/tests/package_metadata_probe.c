#define _GNU_SOURCE
#include "../ports/shared-android/runtime/nxpkg.c"
#include "../native/adapters/title_adapter.h"
#include "../ports/shared-android/runtime/native_package_metadata.h"

#include <inttypes.h>


static void p32(uint8_t *p,uint32_t v){memcpy(p,&v,4);}
static void p64(uint8_t *p,uint64_t v){memcpy(p,&v,8);}
static int tempfile(void){char name[]="/tmp/raitchi-metadata-XXXXXX";int fd=mkstemp(name);if(fd>=0)unlink(name);return fd;}
static bool write_at(int fd,const void*p,size_t n,uint64_t off){return pwrite(fd,p,n,(off_t)off)==(ssize_t)n;}
static void json_string(const char*s){putchar('"');for(const unsigned char*p=(const unsigned char*)s;*p;p++){if(*p=='"'||*p=='\\')printf("\\%c",*p);else if(*p<0x20)printf("\\u%04x",*p);else putchar(*p);}putchar('"');}

static int synthetic_image(uint8_t **out,size_t *out_size){
    const char title[]="Metadata fixture",author[]="Test publisher",version[]="2.7.1";
    const char icon_name[]="icon_AmericanEnglish.dat";const uint8_t icon[]={0x89,'P','N','G',13,10,26,10,0x44,0x55};
    const size_t nacp_size=0x4000;uint8_t*nacp=calloc(1,nacp_size);if(!nacp)return -1;
    memcpy(nacp,title,sizeof title-1);memcpy(nacp+0x200,author,sizeof author-1);memcpy(nacp+0x3060,version,sizeof version-1);
    size_t file_meta=32+sizeof("control.nacp")-1;file_meta=(file_meta+3)&~(size_t)3;file_meta+=32+sizeof icon_name-1;file_meta=(file_meta+3)&~(size_t)3;
    size_t data_off=0x50+24+file_meta;data_off=(data_off+15)&~(size_t)15;size_t total=data_off+nacp_size+sizeof icon;
    uint8_t*b=calloc(1,total);if(!b){free(nacp);return -1;}
    p64(b,0x50);p64(b+24,0x50);p64(b+32,24);p64(b+56,0x68);p64(b+64,file_meta);p64(b+72,data_off);
    p32(b+0x50,UINT32_MAX);p32(b+0x54,UINT32_MAX);p32(b+0x58,UINT32_MAX);p32(b+0x5c,0);p32(b+0x60,UINT32_MAX);p32(b+0x64,0);
    uint8_t*e=b+0x68;p32(e,0);size_t second=32+sizeof("control.nacp")-1;second=(second+3)&~(size_t)3;p32(e+4,(uint32_t)second);p64(e+8,0);p64(e+16,nacp_size);p32(e+24,UINT32_MAX);p32(e+28,sizeof("control.nacp")-1);memcpy(e+32,"control.nacp",sizeof("control.nacp")-1);
    e=b+0x68+second;p32(e,0);p32(e+4,UINT32_MAX);p64(e+8,nacp_size);p64(e+16,sizeof icon);p32(e+24,UINT32_MAX);p32(e+28,sizeof icon_name-1);memcpy(e+32,icon_name,sizeof icon_name-1);
    memcpy(b+data_off,nacp,nacp_size);memcpy(b+data_off+nacp_size,icon,sizeof icon);free(nacp);
    *out=b;*out_size=total;return 0;
}
static NxPackage synthetic_package(uint8_t *romfs,size_t romfs_size,uint8_t *exe,size_t exe_size,int fd){
    NxPackage p={0};p.fd=fd;p.title_id=UINT64_C(0x01008cf01baac000);p.has_control=true;
    p.control.base=0;p.control_romfs=(Section){.off=0,.size=romfs_size,.data_off=0,.data_size=romfs_size,.enc_type=1};
    p.nca.base=0;p.exefs=(Section){.off=0x10000,.size=exe_size,.data_off=0,.data_size=exe_size,.enc_type=1};return p;
}
static bool synthetic_exefs(uint8_t **out,size_t *size,uint8_t build[32]){
    const char name[]="main";const size_t off=64,total=off+0x60;uint8_t*b=calloc(1,total);if(!b)return false;
    memcpy(b,"PFS0",4);p32(b+4,1);p32(b+8,sizeof name);p64(b+16,19);p64(b+24,0x60);p32(b+32,0);memcpy(b+40,name,sizeof name);
    uint8_t*nso=b+off;memcpy(nso,"NSO0",4);for(size_t i=0;i<32;i++)nso[0x40+i]=build[i]=(uint8_t)(0xa0+i);
    *out=b;*size=total;return true;
}
static unsigned tests;
#define T(x) do{tests++;if(!(x)){fprintf(stderr,"metadata probe failed at line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int run_synthetic(void){
    uint8_t*rom=NULL,*exe=NULL;size_t rn=0,en=0;uint8_t build[32];T(!synthetic_image(&rom,&rn));T(synthetic_exefs(&exe,&en,build));
    int fd=tempfile();T(fd>=0);T(write_at(fd,rom,rn,0));T(write_at(fd,exe,en,0x10000));
    NxPackage p=synthetic_package(rom,rn,exe,en,fd);
    NxPackageMetadata m={0};char why[256];bool good=nx_package_read_metadata(&p,&m,why,sizeof why);if(!good)fprintf(stderr,"synthetic metadata: %s\n",why);T(good);T(!strcmp(m.title,"Metadata fixture"));T(!strcmp(m.developer,"Test publisher"));T(!strcmp(m.version,"2.7.1"));if(m.icon_size!=10||!m.icon||memcmp(m.icon,"\x89PNG\r\n\x1a\nDU",10))fprintf(stderr,"icon size=%zu ptr=%p bytes=%02x %02x %02x %02x\n",m.icon_size,(void*)m.icon,m.icon?m.icon[0]:0,m.icon?m.icon[1]:0,m.icon?m.icon[2]:0,m.icon?m.icon[3]:0);T(m.icon_size==10&&!memcmp(m.icon,"\x89PNG\r\n\x1a\nDU",10));T(!memcmp(m.main_build_id,build,32));
    uint8_t id[32];T(nx_package_exefs_build_id(&p,"main",id)&&!memcmp(id,build,32));T(!nx_package_exefs_build_id(&p,"sdk",id));nx_package_metadata_clear(&m);
    /* Invalid RomFS file-data extent, invalid root child pointer, and empty NACP are rejected. */
    uint64_t bad=UINT64_MAX;T(write_at(fd,&bad,8,0x68+8));T(!nx_package_read_metadata(&p,&m,why,sizeof why));
    T(write_at(fd,rom,rn,0));uint32_t badid=0xfffffff0;T(write_at(fd,&badid,4,0x5c));T(!nx_package_read_metadata(&p,&m,why,sizeof why));
    T(write_at(fd,rom,rn,0));memset(rom+(size_t)rd64(rom+72),0,0x4000);T(write_at(fd,rom,rn,0));T(!nx_package_read_metadata(&p,&m,why,sizeof why));
    free(rom);free(exe);close(fd);printf("{\"state\":\"passed\",\"checks\":%u,\"synthetic_romfs_nacp_icon_exefs\":true}\n",tests);return 0;
}
static int scan_real(const char*package_path,const char*keys_path,const char*icon_path){
    int fd=open(package_path,O_RDONLY|O_CLOEXEC);if(fd<0){perror("package open");return 2;}char why[256]={0};
    NxPackage*p=nx_package_open_fd(fd,keys_path,why,sizeof why);close(fd);if(!p){fprintf(stderr,"package scan failed: %s\n",why);return 2;}
    NxPackageMetadata m={0};if(!nx_package_read_metadata(p,&m,why,sizeof why)){fprintf(stderr,"metadata scan failed: %s\n",why);nx_package_close(p);return 2;}
    /* Production launcher classification (specific adapters, then generic tryability). */
    NativePackageClass c;native_package_classify(p,&m,&c);const char*reason=c.reason;bool supported=c.compatible;
    if(m.icon_size&&icon_path){int out=open(icon_path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);if(out<0||write(out,m.icon,m.icon_size)!=(ssize_t)m.icon_size){if(out>=0)close(out);fprintf(stderr,"icon receipt write failed\n");nx_package_metadata_clear(&m);nx_package_close(p);return 2;}close(out);}
    printf("{\"programId\":\"%" PRIu64 "\",\"programIdHex\":\"%016" PRIx64 "\",\"title\":",nx_package_title_id(p),nx_package_title_id(p));json_string(m.title);
    printf(",\"developer\":");json_string(m.developer);printf(",\"version\":");json_string(m.version);printf(",\"adapterId\":");json_string(c.adapter_id);printf(",\"compatible\":%s,\"tryable\":%s,\"reason\":",supported?"true":"false",c.tryable?"true":"false");json_string(reason);
    printf(",\"mainBuildId\":\"");for(size_t i=0;i<32;i++)printf("%02x",m.main_build_id[i]);printf("\",\"iconBytes\":%zu}\n",m.icon_size);
    nx_package_metadata_clear(&m);nx_package_close(p);return 0;
}
int main(int argc,char**argv){if(argc==2&&!strcmp(argv[1],"--synthetic"))return run_synthetic();if(argc==4)return scan_real(argv[1],argv[2],argv[3]);fprintf(stderr,"usage: %s --synthetic | package.nsp prod.keys icon-output\n",argv[0]);return 2;}
