/* Synthetic package/crypto/CompressedStorage tests; no game assets or keys. */
#define _GNU_SOURCE
#include "../ports/shared-android/runtime/nxpkg.c"
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void put32(void *p,uint32_t x){memcpy(p,&x,4);}
static void put64(void *p,uint64_t x){memcpy(p,&x,8);}
static void vector(const void *p,const char *want){uint8_t b[256];size_t n=strlen(want)/2;CHECK(unhex(want,b,sizeof b)==(int)n);CHECK(!memcmp(p,b,n));}
static void crypto(void){
 uint8_t k[32],b[512]={0},iv[16];unhex("000102030405060708090a0b0c0d0e0f",k,32);unhex("69c4e0d86a7b0430d8cdb78070b4c55a",b,512);nx_aes128_ecb_decrypt(k,b,16);vector(b,"00112233445566778899aabbccddeeff");
 unhex("2b7e151628aed2a6abf7158809cf4f3c",k,32);unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",iv,16);unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52ef",b,512);nx_aes128_ctr(k,iv,b,45);vector(b,"874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f0902");
 memset(k,0,32);memset(b,0,512);unhex("917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e",b,512);aes_xts_decrypt(k,b,512,0);vector(b,"0000000000000000000000000000000000000000000000000000000000000000");
 uint8_t lit[]={0x40,1,2,3,4};CHECK(nx_lz4_decompress(lit,5,b,512)==4&&!memcmp(b,lit+1,4));uint8_t overlap[]={0x10,'x',1,0};CHECK(nx_lz4_decompress(overlap,4,b,512)==5&&!memcmp(b,"xxxxx",5));uint8_t bad[]={0,0,0};CHECK(nx_lz4_decompress(bad,3,b,512)==-1);CHECK(nx_lz4_decompress(lit,5,b,3)==-1);CHECK(nx_lz4_decompress(lit,-1,b,3)==-1);CHECK(nx_lz4_decompress(NULL,0,NULL,0)==0);CHECK(unhex("0q",b,32)==0);
}
static int tempfd(void){char p[]="/tmp/nxpkg-data-XXXXXX";int fd=mkstemp(p);CHECK(fd>=0);CHECK(!unlink(p));return fd;}
static void writeat(int fd,const void*p,size_t n,uint64_t off){CHECK(pwrite(fd,p,n,(off_t)off)==(ssize_t)n);}
static void xts_encrypt(uint8_t *p){uint8_t rk[176],key[16]={0};aes_expand(key,rk);for(unsigned s=0;s<6;s++){uint8_t tw[16]={0};tw[15]=(uint8_t)s;aes_encrypt_block(rk,tw);for(unsigned off=0;off<512;off+=16){uint8_t*b=p+s*512+off;for(unsigned i=0;i<16;i++)b[i]^=tw[i];aes_encrypt_block(rk,b);for(unsigned i=0;i<16;i++)b[i]^=tw[i];uint8_t carry=tw[15]>>7;for(int i=15;i>0;i--)tw[i]=(uint8_t)(tw[i]<<1|tw[i-1]>>7);tw[0]=(uint8_t)(tw[0]<<1)^(carry?0x87:0);}}}
static int package_file(bool nsz){
 int fd=tempfd();uint8_t *buf=calloc(1,0x5000);CHECK(buf);memcpy(buf,"PFS0",4);put32(buf+4,1);put32(buf+8,12);put64(buf+16+8,0x4000);memcpy(buf+40,nsz?"program.ncz":"program.nca",12);
 uint8_t *h=buf+52;memcpy(h+0x200,"NCA3",4);put64(h+0x210,0x100000000000000ull);put32(h+0x240,8);put32(h+0x244,16);put32(h+0x250,16);put32(h+0x254,24);
 for(unsigned i=0;i<2;i++){uint8_t *f=h+0x400+i*0x200;f[2]=(uint8_t)(i==0);f[3]=2;f[4]=1;put32(f+0x2c,1);put64(f+0x38,0x1000);}
 uint8_t rk[176],zero[16]={0};aes_expand(zero,rk);for(unsigned i=0;i<0x40;i+=16)aes_encrypt_block(rk,h+0x300+i);xts_encrypt(h);
 uint8_t *e=h+0x1000;memcpy(e,"PFS0",4);put32(e+4,1);put32(e+8,5);put64(e+24,4);memcpy(e+40,"main",5);memcpy(e+45,"TEST",4);put64(h+0x2000,0x50);
 writeat(fd,buf,0x5000,0);free(buf);return fd;
}
static void package_tests(void){
 char root[]="/tmp/nxpkg-test-XXXXXX";CHECK(mkdtemp(root));char keys[256],exefs[256],mainfile[280],why[256];snprintf(keys,sizeof keys,"%s/prod.keys",root);FILE*f=fopen(keys,"w");CHECK(f);CHECK(fputs("header_key = 0000000000000000000000000000000000000000000000000000000000000000\nkey_area_key_application_00 = 00000000000000000000000000000000\n",f)>0);CHECK(!fclose(f));
 int fd=package_file(false);CHECK(lseek(fd,17,SEEK_SET)==17);NxPackage*p=nx_package_open_fd(fd,keys,why,sizeof why);if(!p)fprintf(stderr,"open failed: %s\n",why);CHECK(p);CHECK(nx_package_title_id(p)==0x100000000000000ull);CHECK(lseek(fd,0,SEEK_CUR)==17);CHECK(!close(fd));
 NxRomfs*r=nx_package_open_romfs(p,why,sizeof why);CHECK(r&&nx_romfs_size(r)==0x1000);uint64_t magic=0;CHECK(nx_romfs_pread(r,&magic,8,0)==8&&magic==0x50);
 snprintf(exefs,sizeof exefs,"%s/exefs",root);CHECK(nx_package_extract_exefs(p,exefs,why,sizeof why));CHECK(nx_package_extract_exefs(p,exefs,why,sizeof why));snprintf(mainfile,sizeof mainfile,"%s/main",exefs);f=fopen(mainfile,"rb");CHECK(f);char value[4];CHECK(fread(value,1,4,f)==4&&!memcmp(value,"TEST",4));CHECK(!fclose(f));
 f=fopen(mainfile,"wb");CHECK(f);CHECK(fwrite("KEEP",1,4,f)==4);CHECK(!fclose(f));CHECK(!nx_package_extract_exefs(p,exefs,why,sizeof why));f=fopen(mainfile,"rb");CHECK(f);CHECK(fread(value,1,4,f)==4&&!memcmp(value,"KEEP",4));CHECK(!fclose(f));
 // Complete ExeFS validation precedes writes, including hostile flat names.
 char badname[5]="../x";writeat(p->fd,badname,sizeof badname,p->nca.base+p->exefs.off+40);CHECK(!nx_package_extract_exefs(p,exefs,why,sizeof why));
 writeat(p->fd,"main",5,p->nca.base+p->exefs.off+40);uint64_t overflow=UINT64_MAX;writeat(p->fd,&overflow,8,p->nca.base+p->exefs.off+16);CHECK(!nx_package_extract_exefs(p,exefs,why,sizeof why));
 nx_package_close(p);CHECK(nx_romfs_pread(r,&magic,8,0)==8&&magic==0x50);CHECK(nx_romfs_pread(r,value,4,0xfff)==1);CHECK(nx_romfs_pread(r,value,4,0x1000)==0);CHECK(nx_romfs_pread(r,NULL,1,0)==-1);
 NxRomfs*copy=nx_romfs_clone(r,why,sizeof why);CHECK(copy&&copy->fd!=r->fd&&!copy->ent);nx_romfs_close(r);CHECK(nx_romfs_pread(copy,&magic,8,0)==8&&magic==0x50);nx_romfs_close(copy);CHECK(!nx_romfs_clone(NULL,why,sizeof why));
 fd=package_file(true);CHECK(!nx_package_open_fd(fd,keys,why,sizeof why)&&strstr(why,"NSZ"));close(fd);
 CHECK(!nx_package_open_fd(-1,keys,why,sizeof why));CHECK(!nx_package_open("/path/absent",keys,why,sizeof why));
 // Package table offsets and strings must fail before NCA parsing.
 fd=package_file(false);uint64_t bad=UINT64_MAX;writeat(fd,&bad,8,16);CHECK(!nx_package_open_fd(fd,keys,why,sizeof why));close(fd);
 fd=package_file(false);uint32_t huge=UINT32_MAX;writeat(fd,&huge,4,8);CHECK(!nx_package_open_fd(fd,keys,why,sizeof why));close(fd);
 CHECK(!unlink(mainfile));CHECK(!rmdir(exefs));CHECK(!unlink(keys));CHECK(!rmdir(root));
}
static NxRomfs *compressed(int mutation){
 NxRomfs*r=calloc(1,sizeof*r);CHECK(r);r->fd=tempfd();CHECK(!pthread_mutex_init(&r->lock,NULL));r->sec.enc_type=1;r->sec.size=r->sec.data_size=0xc000;r->sec.comp_off=0x4000;r->sec.comp_size=0x8000;memcpy(r->sec.comp_hdr,"BKTR",4);put32(r->sec.comp_hdr+8,60);
 uint8_t *buf=calloc(1,0xc000);CHECK(buf);put32(buf+0x8000+4,60);put64(buf+0x8000+8,960);
 for(unsigned i=0;i<60;i++){uint8_t *e=buf+0x8010+i*24;put64(e,i*16);put64(e+8,i*32);e[16]=(uint8_t)(i%3==0?0:i%3==1?1:3);put32(e+20,18);if(e[16]==3){buf[i*32]=0xf0;buf[i*32+1]=1;memset(buf+i*32+2,(int)i,16);}else memset(buf+i*32,(int)i,16);}
 if(mutation==1)put32(r->sec.comp_hdr+8,0);
 if(mutation==2)put64(buf+0x8010+24,0);
 if(mutation==3)buf[0x8010+16]=2;
 if(mutation==4)put64(buf+0x8010+8,UINT64_MAX);
 if(mutation==5)put64(buf+0x8010+8,0xbfff);
 if(mutation==6)put32(buf+0x8010+48+20,MAX_BLOCK+1);
 if(mutation==7)put32(buf+0x8000+4,19);
 writeat(r->fd,buf,0xc000,0);free(buf);return r;
}
typedef struct{NxRomfs*r;int failed;} Worker;
static void *reader(void *arg){Worker*w=arg;uint8_t data[49];for(unsigned i=0;i<2000;i++){unsigned off=(i*19)%940,n=(i%48)+1;if(n>960-off)n=960-off;if(nx_romfs_pread(w->r,data,n,off)!=(ssize_t)n){w->failed=1;break;}for(unsigned j=0;j<n;j++){unsigned block=(off+j)/16;uint8_t want=block%3==1?0:(uint8_t)block;if(data[j]!=want)w->failed=1;}}return NULL;}
static void compressed_tests(void){
 char why[256];for(int i=1;i<=7;i++){NxRomfs*r=compressed(i);CHECK(!comp_load(r,why,sizeof why));nx_romfs_close(r);}
 NxRomfs*r=compressed(0);CHECK(comp_load(r,why,sizeof why));CHECK(nx_romfs_size(r)==960);uint8_t data[16];CHECK(nx_romfs_pread(r,data,16,32)==16);
 NxRomfs*copy=nx_romfs_clone(r,why,sizeof why);CHECK(copy&&copy->fd!=r->fd&&copy->ent!=r->ent&&copy->nent==r->nent&&!memcmp(copy->ent,r->ent,(r->nent+1)*sizeof*r->ent));for(unsigned i=0;i<NCACHE;i++)CHECK(!copy->cache[i].data);nx_romfs_close(r);r=copy;
 Worker w[4];pthread_t threads[4];for(unsigned i=0;i<4;i++){w[i]=(Worker){r,0};CHECK(!pthread_create(&threads[i],NULL,reader,&w[i]));}for(unsigned i=0;i<4;i++){CHECK(!pthread_join(threads[i],NULL));CHECK(!w[i].failed);}CHECK(nx_romfs_pread(r,data,16,952)==8);nx_romfs_close(r);
 r=compressed(0);CHECK(comp_load(r,why,sizeof why));uint8_t invalid=0;writeat(r->fd,&invalid,1,64);CHECK(nx_romfs_pread(r,data,16,32)==-1);nx_romfs_close(r);
}
int main(void){crypto();package_tests();compressed_tests();printf("{\"state\":\"passed\",\"checks\":%u,\"threaded_reads\":8000,\"synthetic_encrypted_header\":true,\"no_romfs_extraction\":true}\n",checks);return 0;}
