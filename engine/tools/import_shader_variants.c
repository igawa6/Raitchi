/* Metadata-only adaptation of upstream shader-tour records. No original code is
 * written to disk: source spans are verified and released after each recipe. */
#define _GNU_SOURCE
#include "shader_profile.h"
#include "shader_source_index.h"
#include <ctype.h>
#include <time.h>
#include "variant_import_production.inc"
bool shared_sha256_bytes(const void *, size_t, uint8_t[32]);
#define IMPORT_MAX_LINE 4096u
#define IMPORT_MAX_TEXT (16u * 1024u * 1024u)
typedef struct { size_t lines,graphics,compute,missing,invalid,duplicates; } ImportStats;
static bool value_chars(const char *s, const char *extra, bool decimal) {
    for (; *s; s++) if (!(decimal ? isdigit((unsigned char)*s) : isxdigit((unsigned char)*s)) && !strchr(extra,*s)) return false;
    return true;
}
/* Guard the old parser's fixed-width blend reader BEFORE invoking it. The
 * canonical round trip rejects overflow, unknown/duplicate/missing fields,
 * extra tokens, ignored separators and unsupported legacy spellings. */
static bool import_parse(const char *line, ShaderRec *out) {
    size_t len = strlen(line);
    if (!len || len >= IMPORT_MAX_LINE || line[0] != 'G' && line[0] != 'C') return false;
    char copy[IMPORT_MAX_LINE]; memcpy(copy,line,len+1);
    const char *g[] = {"vs","fs","f","a","s","c","d","n","m","b","B","k","t","p"};
    const char *c[] = {"cs","l"}; const char **names=line[0]=='G'?g:c;size_t count=line[0]=='G'?14:2;
    char *save=NULL,*token=strtok_r(copy," ",&save);if(!token || strlen(token)!=1)return false;
    unsigned blends=0,segments=0;
    for(size_t k=0;k<count;k++) {
        token=strtok_r(NULL," ",&save);if(!token)return false;
        size_t n=strlen(names[k]);if(strncmp(token,names[k],n)||token[n]!='=')return false;
        char *v=token+n+1;
        bool decimal=line[0]=='C'&&k==1;
        if(!value_chars(v,decimal?",":",/:",decimal))return false;
        if(line[0]=='G'&&k==9){if(strlen(v)>2||!*v)return false;blends=(unsigned)strtoul(v,NULL,16);}
        if(line[0]=='G'&&k==10) {
            if(*v){char *part=v;for(;;){char *end=strchr(part,',');size_t size=end?(size_t)(end-part):strlen(part);if(size!=16)return false;segments++;if(!end)break;part=end+1;}}
            if(segments!=(unsigned)__builtin_popcount(blends))return false;
        }
    }
    if(strtok_r(NULL," ",&save)||!shader_rec_parse(line,out))return false;
    char canonical[IMPORT_MAX_LINE];shader_rec_format(out,canonical,sizeof canonical);
    return !strcmp(line,canonical)&&nvn_disc_rec_valid(out);
}
/* import_keys is generated from the production renderer_prepare key block.
 * Its zero tex_uint is the explicit upstream float-sampled specialization;
 * exact renderer keys prevent UINT bindings from reusing these candidates. */
static bool import_entry(ShaderSourceIndex *idx,const ShaderRec *r,ShaderProfileEntry *e) {
    memset(e,0,sizeof *e);e->rec=*r;import_keys(r,&e->vkey,&e->fkey);
    for(unsigned i=0;i<2;i++) {
        uint64_t hash=i?r->fs:r->vs;if(!hash)continue;
        ShaderSourceSpan span;uint8_t stage=i?4:r->kind=='C'?5:0;
        if(!shader_source_index_lookup(idx,hash,&span)||!(span.stage_mask&(1u<<stage))||!span.const_offset)return false;
        uint8_t *data=shader_source_index_read_exact(idx,hash,stage,span.const_offset,span.const_size,NULL);
        if(!data)return false;
        bool valid=nvn_disc_codehash(data,span.size)==hash;free(data);if(!valid)return false;
        e->code_bytes[i]=span.const_offset;e->constant_bytes[i]=span.const_size;
    }
    return shader_profile_entry_valid(e);
}
static bool import_text(const char *path,ShaderSourceIndex *idx,ShaderProfile *p,ImportStats *stats) {
    FILE *f=fopen(path,"rb");if(!f)return false;struct stat st;
    if(fstat(fileno(f),&st)||st.st_size<0||st.st_size>IMPORT_MAX_TEXT){fclose(f);return false;}
    char line[IMPORT_MAX_LINE];bool ok=true;
    size_t consumed=0;
    for(;;) {
        off_t before=ftello(f);if(before<0){ok=false;break;}
        if(!fgets(line,sizeof line,f))break;
        off_t after=ftello(f);size_t n=strlen(line);
        if(after<before||(uint64_t)(after-before)!=n||n>IMPORT_MAX_TEXT-consumed){stats->invalid++;ok=false;break;}
        consumed+=n;if(n&&line[n-1]=='\n')line[--n]=0;
        if(!n||n==sizeof line-1){ok=false;stats->invalid++;break;}
        if(++stats->lines>NVN_DISCOVERY_MAX_RECORDS){ok=false;break;}
        ShaderRec rec;if(!import_parse(line,&rec)){stats->invalid++;ok=false;break;}
        ShaderProfileEntry e;if(!import_entry(idx,&rec,&e)){stats->missing++;continue;}
        bool duplicate=false;for(size_t i=0;i<p->count;i++)if(!memcmp(&p->entries[i],&e,sizeof e)){duplicate=true;break;}
        if(duplicate){stats->duplicates++;continue;}
        p->entries[p->count++]=e;if(rec.kind=='C')stats->compute++;else stats->graphics++;
    }
    if(ferror(f))ok=false;if(fclose(f))ok=false;return ok;
}
typedef struct {ShaderSourceIndex *index;size_t reads;} ImportResolver;
static bool import_resolve(void *context,unsigned stage,uint64_t hash,uint32_t code,uint32_t constants,uint8_t **out) {
    ImportResolver *resolver=context;resolver->reads++;
    *out=shader_source_index_read_exact(resolver->index,hash,stage==1?4:(uint8_t)stage,code,constants,NULL);
    return *out!=NULL;
}
#ifndef VARIANT_IMPORT_TEST
static bool game_identity(const char *hex,uint8_t out[32]) {
    if(strlen(hex)!=64)return false;
    for(unsigned i=0;i<32;i++){if(!isxdigit((unsigned char)hex[i*2])||!isxdigit((unsigned char)hex[i*2+1]))return false;char b[3]={hex[i*2],hex[i*2+1],0};out[i]=(uint8_t)strtoul(b,NULL,16);}return true;
}
int main(int argc,char **argv) {
    /* Positional ABI is deliberately private to the argparse wrapper. */
    if(argc!=9)return 2;ShaderProfile profile={0};if(!game_identity(argv[2],profile.game_id))return 2;
    ShaderSourceIndex *index=shader_source_index_create();profile.entries=calloc(NVN_DISCOVERY_MAX_RECORDS,sizeof *profile.entries);
    if(!index||!profile.entries)return 3;
    char why[256]={0};bool complete=true;NxPackage *package=NULL;NxRomfs *romfs=NULL;
    if(strcmp(argv[4],"-"))complete=shader_source_index_nso(index,argv[4],NULL,why,sizeof why)&&complete;
    if(strcmp(argv[5],"-"))complete=shader_source_index_directory(index,argv[5],NULL,why,sizeof why)&&complete;
    if(strcmp(argv[6],"-")) {
        package=nx_package_open(argv[6],argv[7],why,sizeof why);
        if(package)romfs=nx_package_open_romfs(package,why,sizeof why);
        if(!romfs)complete=false;else complete=shader_source_index_romfs(index,romfs,NULL,why,sizeof why)&&complete;
    }
    ImportStats stats={0};bool ok=import_text(argv[1],index,&profile,&stats);
    if(!stats.graphics&&!stats.compute)ok=false;
    NvnDiscoveryStore empty={0};if(ok)ok=nvn_discovery_init_empty(&empty,"/unused-no-journal-commit",profile.game_id,shared_sha256_bytes,32u*1024u*1024u,4096);
    size_t observed_duplicates=0;
    ShaderProfile observed={0};
    if(ok&&strcmp(argv[8],"-")) {
        if(shader_profile_load(argv[8],profile.game_id,shared_sha256_bytes,&observed)!=SHADER_PROFILE_LOADED)ok=false;
        else for(size_t i=0;ok&&i<observed.count;i++) {
            ShaderProfileEntry *e=&observed.entries[i];bool dup=false;for(size_t j=0;j<profile.count;j++)if(!memcmp(e,&profile.entries[j],sizeof *e)){dup=true;break;}
            if(dup)observed_duplicates++;
            if(!dup){if(profile.count==NVN_DISCOVERY_MAX_RECORDS)ok=false;else profile.entries[profile.count++]=*e;}
        }
    }
    NvnDiscoveryStore verified={0};ImportResolver resolver={index,0};ShaderProfileMergeStats cold={0},warm={0};size_t cold_reads=0,warm_reads=0;
    if(ok)ok=nvn_discovery_init_empty(&verified,"/unused-source-verification-no-commit",profile.game_id,shared_sha256_bytes,32u*1024u*1024u,4096);
    if(ok) {
        ok=shader_profile_merge(&profile,&verified,import_resolve,&resolver,NULL,&cold);
        cold_reads=resolver.reads;resolver.reads=0;
        if(ok)ok=shader_profile_merge(&profile,&verified,import_resolve,&resolver,NULL,&warm);
        warm_reads=resolver.reads;
        ok=ok&&cold.appended==profile.count&&!cold.missing&&!cold.rejected&&!warm.appended&&warm.reused==profile.count&&!warm_reads;
    }
    if(ok)ok=shader_profile_import_union(argv[3],&empty,&profile);
    ShaderProfile written={0};
    if(ok)ok=shader_profile_load(argv[3],profile.game_id,shared_sha256_bytes,&written)==SHADER_PROFILE_LOADED;
    printf("{\"state\":\"%s\",\"upstream_records\":%zu,\"resolved_graphics\":%zu,\"resolved_compute\":%zu,\"missing_or_incompatible\":%zu,\"invalid_lines\":%zu,\"duplicates\":%zu,\"observed_records\":%zu,\"observed_duplicates\":%zu,\"input_union_records\":%zu,\"union_records\":%zu,\"indexed_stages\":%zu,\"cold_verified_recipes\":%zu,\"cold_resolver_reads\":%zu,\"cold_missing\":%zu,\"cold_rejected\":%zu,\"warm_reused\":%zu,\"warm_resolver_reads\":%zu,\"source_scan_complete\":%s,\"upstream_texture_specialization\":\"tex_uint=0 only; actual binding coverage not inferred\",\"shader_payload_included\":false}\n",ok?"written":"failed",stats.lines,stats.graphics,stats.compute,stats.missing,stats.invalid,stats.duplicates,observed.count,observed_duplicates,profile.count,written.count,shader_source_index_count(index),cold.appended,cold_reads,cold.missing,cold.rejected,warm.reused,warm_reads,complete?"true":"false");
    shader_profile_close(&written);shader_profile_close(&observed);nvn_discovery_close(&verified);nvn_discovery_close(&empty);shader_source_index_destroy(index);nx_romfs_close(romfs);nx_package_close(package);shader_profile_close(&profile);return ok?0:1;
}
#endif
