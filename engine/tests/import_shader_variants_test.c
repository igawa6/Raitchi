/* Synthetic original containers; exercises actual importer, codec and index. */
#define VARIANT_IMPORT_TEST
#include "../tools/import_shader_variants.c"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static void put32(uint8_t*p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(i*8));}
static void put64(uint8_t*p,uint64_t x){put32(p,(uint32_t)x);put32(p+4,(uint32_t)(x>>32));}
static uint64_t fixture(uint8_t *p,unsigned stage,uint32_t code,uint32_t constants,unsigned seed){
 memcpy(p,"BNSH\0\0\0\0",8);p[12]=0xff;p[13]=0xfe;p[22]=0x40;put32(p+28,0x1000);
 memcpy(p+0x40,"grsc",4);put32(p+0x5c,1);put64(p+0x60,0x80);put64(p+0x90,0x100);
 put64(p+0x108+stage*8,0x140);put64(p+0x148,0x180);put64(p+0x150,0x900);put32(p+0x158,code+constants);
 put32(p+0x180+0x6fc,constants);put32(p+0x180+0x700,code);
 for(unsigned i=0;i<code+constants;i++)p[0x900+i]=(uint8_t)(seed+i*7+i/3);
 return nvn_disc_codehash(p+0x900,code+constants);
}
typedef struct {uint8_t*bytes;size_t size;} Mem;
static bool read_mem(void *ctx,uint64_t at,void*out,size_t n){Mem*m=ctx;CHECK(at<=m->size&&n<=m->size-at);memcpy(out,m->bytes+at,n);return true;}
static void save(const char*p,const char*s){FILE*f=fopen(p,"wb");CHECK(f);CHECK(fwrite(s,1,strlen(s),f)==strlen(s));CHECK(!fclose(f));}
int main(int argc,char**argv){CHECK(argc==2);CHECK(!chdir(argv[1]));
 uint8_t *bytes=calloc(1,0x4000);CHECK(bytes);uint64_t v=fixture(bytes,0,128,16,0x21),f=fixture(bytes+0x1000,4,128,8,0x49),c=fixture(bytes+0x2000,5,32,8,0xc5);
 FILE *archive=fopen("fixture.bfsha","wb");CHECK(archive);CHECK(fwrite(bytes,1,0x4000,archive)==0x4000);CHECK(!fclose(archive));
 Mem mem={bytes,0x4000};ShaderSourceIndex*idx=shader_source_index_create();CHECK(idx);char why[256];CHECK(shader_source_index_scan(idx,read_mem,&mem,mem.size,0,mem.size,NULL,why,sizeof why));CHECK(shader_source_index_count(idx)==3);
 ShaderRec rec={.kind='G',.vs=v,.fs=f,.flags=7,.nattrib=2,.nstream=1,.ncolor=1,.samples=1,.channel_mask=15,.blend_on=1,.topo=2,.poly_fill=1};rec.attrib[0]=0x03;rec.attrib[1]=0x04;rec.stream[0][0]=16;rec.color[0]=0x14;
 char line[4096],bad[4096];shader_rec_format(&rec,line,sizeof line);ShaderRec parsed;CHECK(import_parse(line,&parsed));CHECK(!memcmp(&parsed,&rec,sizeof rec));
 ShaderProfileEntry e;CHECK(import_entry(idx,&rec,&e));CHECK(e.code_bytes[0]==128&&e.constant_bytes[0]==16&&e.code_bytes[1]==128&&e.constant_bytes[1]==8);CHECK(e.vkey.attr_kind[0]==2&&e.vkey.attr_kind[1]==1&&e.fkey.rt_kind[0]==2);CHECK(e.vkey.flip_x&&e.vkey.flip_y&&e.vkey.depth_minus_one&&e.fkey.flip_x&&e.fkey.flip_y&&!e.fkey.depth_minus_one);CHECK(!e.vkey.vs_out_mask&&!e.fkey.vs_out_mask&&!e.vkey.tex_uint&&!e.fkey.tex_uint);
 snprintf(bad,sizeof bad,"%s x=0",line);CHECK(!import_parse(bad,&parsed));snprintf(bad,sizeof bad,"%s vs=0000000000000001",line);CHECK(!import_parse(bad,&parsed));CHECK(!import_parse("C cs=0000000000000001 l=1,2,3 x=0",&parsed));CHECK(!import_parse("C cs=0000000000000001 l=4294967296,1,1",&parsed));CHECK(!import_parse("C cs=0000000000000001 l=0,1,1",&parsed));CHECK(!import_parse("C cs=0000000000000001 l=1,1,1,1",&parsed));CHECK(!import_parse("C cs=0000000000000001 l=1,1",&parsed));
 const char*short_blend="G vs=0000000000000001 fs=0000000000000002 f=0 a= s= c= d=0 n=1 m=f b=ff B=00 k=0 t=2 p=1";CHECK(!import_parse(short_blend,&parsed));CHECK(!import_parse("G vs=1 fs=2 f=0 a= s= c= d=0 n=1 m=f b=1 B= k=0 t=2 p=1",&parsed));CHECK(!import_parse("C cs=0000000000000001 l=1/1/1",&parsed));CHECK(!import_parse(" C cs=0000000000000001 l=1,1,1",&parsed));
 ShaderRec compute={.kind='C',.vs=c,.local={32,1,1}};shader_rec_format(&compute,bad,sizeof bad);CHECK(import_parse(bad,&parsed));CHECK(import_entry(idx,&compute,&e));CHECK(e.vkey.glsl==1&&!e.fkey.glsl&&e.code_bytes[0]==32&&e.constant_bytes[0]==8);
 ShaderRec wrong=compute;wrong.vs=v;CHECK(!import_entry(idx,&wrong,&e));wrong=rec;wrong.fs=c;CHECK(!import_entry(idx,&wrong,&e));wrong=rec;wrong.vs++;CHECK(!import_entry(idx,&wrong,&e));bytes[0x900]^=1;CHECK(!import_entry(idx,&rec,&e));bytes[0x900]^=1;
 char all[16384];snprintf(all,sizeof all,"%s\n%s\n%s\n",line,line,bad);save("valid.txt",all);
 ShaderProfile profile={0};profile.game_id[0]=17;profile.entries=calloc(4096,sizeof*profile.entries);CHECK(profile.entries);ImportStats stats={0};CHECK(import_text("valid.txt",idx,&profile,&stats));CHECK(stats.lines==3&&stats.graphics==1&&stats.compute==1&&stats.duplicates==1&&profile.count==2);
 NvnDiscoveryStore empty={0};CHECK(nvn_discovery_init_empty(&empty,"/unused",profile.game_id,shared_sha256_bytes,32u*1024u*1024u,4096));CHECK(shader_profile_import_union("profile.bin",&empty,&profile));ShaderProfile loaded={0};CHECK(shader_profile_load("profile.bin",profile.game_id,shared_sha256_bytes,&loaded)==SHADER_PROFILE_LOADED&&loaded.count==2);CHECK(!memcmp(&loaded.entries[0],&profile.entries[0],sizeof e));shader_profile_close(&loaded);
 ShaderProfileEntry extra=profile.entries[0];extra.rec.channel_mask=7;ShaderProfile previous=profile;previous.entries=&extra;previous.count=1;CHECK(shader_profile_import_union("profile.bin",&empty,&previous));CHECK(shader_profile_import_union("profile.bin",&empty,&profile));CHECK(shader_profile_load("profile.bin",profile.game_id,shared_sha256_bytes,&loaded)==SHADER_PROFILE_LOADED&&loaded.count==3);shader_profile_close(&loaded);
 save("invalid.txt",short_blend);ImportStats invalid={0};CHECK(!import_text("invalid.txt",idx,&profile,&invalid));CHECK(invalid.invalid==1&&profile.count==2);
 memset(all,'a',sizeof all-1);all[sizeof all-1]=0;save("oversized-line.txt",all);CHECK(!import_text("oversized-line.txt",idx,&profile,&invalid));
 FILE *nul=fopen("nul.txt","wb");CHECK(nul);CHECK(fwrite(line,1,strlen(line),nul)==strlen(line));CHECK(fwrite("\0trailing\n",1,10,nul)==10);CHECK(!fclose(nul));CHECK(!import_text("nul.txt",idx,&profile,&invalid));
 ImportResolver resolver={idx,0};NvnDiscoveryStore verified={0};ShaderProfileMergeStats cold={0},warm={0};CHECK(nvn_discovery_init_empty(&verified,"/unused-verified",profile.game_id,shared_sha256_bytes,32u*1024u*1024u,4096));CHECK(shader_profile_merge(&profile,&verified,import_resolve,&resolver,NULL,&cold));CHECK(cold.appended==2&&!cold.missing&&!cold.rejected&&resolver.reads==3);resolver.reads=0;CHECK(shader_profile_merge(&profile,&verified,import_resolve,&resolver,NULL,&warm));CHECK(warm.reused==2&&!warm.appended&&!resolver.reads);nvn_discovery_close(&verified);
 FILE *huge=fopen("huge.txt","wb");CHECK(huge);CHECK(!ftruncate(fileno(huge),IMPORT_MAX_TEXT+1));CHECK(!fclose(huge));CHECK(!import_text("huge.txt",idx,&profile,&invalid));
 nvn_discovery_close(&empty);shader_profile_close(&profile);shader_source_index_destroy(idx);free(bytes);printf("{\"assertions\":%u,\"synthetic_only\":true,\"production_parser_key_block_codec_source_index\":true}\n",checks);return 0;}
