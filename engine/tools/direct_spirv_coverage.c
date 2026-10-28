/* Host-only admission survey. Input journal is never modified. Generated words
 * are written only into the caller's private temporary directory for validation. */
#include "shader_discovery.h"
#include "direct_spirv.h"
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>

bool shared_sha256_bytes(const void *, size_t, uint8_t[32]);
bool shared_title_las(void) { return false; }

static void quoted(const char *text) {
    putchar('"');
    for (const unsigned char *p=(const unsigned char *)text;*p;p++) {
        if(*p=='"'||*p=='\\'){putchar('\\');putchar(*p);}
        else if(*p<32)printf("\\u%04x",*p);
        else putchar(*p);
    }
    putchar('"');
}
static bool save_words(const char *directory,size_t index,unsigned stage,const MaxwellSpirvOutput *out) {
    char path[4096];int n=snprintf(path,sizeof path,"%s/%zu-%u.spv",directory,index,stage);
    if(n<0||(size_t)n>=sizeof path)return false;
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return false;
    FILE *f=fdopen(fd,"wb");if(!f){close(fd);unlink(path);return false;}
    bool ok=fwrite(out->words,sizeof(uint32_t),out->word_count,f)==out->word_count;
    if(fclose(f))ok=false;if(!ok)unlink(path);return ok;
}
static const char *instruction_class(uint64_t w) {
#define OP(value,mask,name) if((w&UINT64_C(mask))==UINT64_C(value))return name
    OP(0x5c58000000000000,0xfff8000000000000,"FADD");OP(0x4c58000000000000,0xfff8000000000000,"FADD");OP(0x3858000000000000,0xfef8000000000000,"FADD");
    OP(0x5c68000000000000,0xfff8000000000000,"FMUL");OP(0x4c68000000000000,0xfff8000000000000,"FMUL");OP(0x3868000000000000,0xfef8000000000000,"FMUL");
    OP(0x5980000000000000,0xff80000000000000,"FFMA");OP(0x4980000000000000,0xff80000000000000,"FFMA");OP(0x5180000000000000,0xff80000000000000,"FFMA");OP(0x3280000000000000,0xfe80000000000000,"FFMA");
    OP(0x0800000000000000,0xfc00000000000000,"FADD32I");OP(0x1e00000000000000,0xff00000000000000,"FMUL32I");OP(0x0c00000000000000,0xfc00000000000000,"FFMA32I");
    OP(0x5c10000000000000,0xfff8000000000000,"IADD");OP(0x4c10000000000000,0xfff8000000000000,"IADD");OP(0x3810000000000000,0xfef8000000000000,"IADD");OP(0x1c00000000000000,0xfe00000000000000,"IADD32I");
    OP(0x5be0000000000000,0xfff8000000000000,"LOP3");OP(0x3c00000000000000,0xfc00000000000000,"LOP3");OP(0x0200000000000000,0xfe00000000000000,"LOP3");
    OP(0x5c40000000000000,0xfff8000000000000,"LOP");OP(0x4c40000000000000,0xfff8000000000000,"LOP");OP(0x3840000000000000,0xfef8000000000000,"LOP");
    OP(0xef90000000000000,0xfff8000000000000,"LDC");OP(0x5080000000000000,0xfff8000000000000,"MUFU");OP(0xf0c8000000000000,0xfff8000000000000,"S2R");
    OP(0xe240000000000000,0xfff0000000000020,"BRA");OP(0xe250000000000000,0xfff0000000000000,"BRX");OP(0xe290000000000000,0xfff0000000000020,"SSY");
    OP(0x5cb0000000000000,0xfff8000000000000,"F2I");OP(0x4cb0000000000000,0xfff8000000000000,"F2I");OP(0x38b0000000000000,0xfef8000000000000,"F2I");
    OP(0x5cb8000000000000,0xfff8000000000000,"I2F");OP(0x4cb8000000000000,0xfff8000000000000,"I2F");OP(0x38b8000000000000,0xfef8000000000000,"I2F");
    OP(0xefd8000000000000,0xfff8000000000000,"LDA");OP(0xeff0000000000000,0xfff8000000000000,"STA");
    return "other";
#undef OP
}
#ifdef DIRECT_SPIRV_COVERAGE_EDEN_OPCODES
/* Optional read-only public opcode names from the pinned Eden reference. The
 * table retains its upstream SPDX header and is not packaged into the runtime. */
static bool encoding_matches(uint64_t word,const char *encoding) {
    unsigned remaining=64;
    for(const char *p=encoding;*p;p++) {
        if(*p!='0'&&*p!='1'&&*p!='-')continue;
        if(!remaining)return false;
        remaining--;
        if(*p!='-'&&((word>>remaining)&1u)!=(unsigned)(*p-'0'))return false;
    }
    return true;
}
static const char *reference_instruction(uint64_t w) {
#define INST(name,cute,encoding) if(encoding_matches(w,encoding))return #name;
#include DIRECT_SPIRV_COVERAGE_EDEN_OPCODES
#undef INST
    return "unknown";
}
#endif
static void modifier_fields(uint64_t w,const char *name) {
    unsigned rounding=0,fmz=0,scale=0,cc=0;
    if(!strcmp(name,"FADD")){rounding=(unsigned)(w>>39)&3;fmz=(unsigned)(w>>44)&1;cc=(unsigned)(w>>47)&1;}
    else if(!strcmp(name,"FMUL")){rounding=(unsigned)(w>>39)&3;scale=(unsigned)(w>>41)&7;fmz=(unsigned)(w>>44)&3;cc=(unsigned)(w>>47)&1;}
    else if(!strcmp(name,"FFMA")){rounding=(unsigned)(w>>51)&3;fmz=(unsigned)(w>>53)&3;cc=(unsigned)(w>>47)&1;}
    else return;
    printf(",\"rejected_modifiers\":{\"rounding\":%u,\"flush_mode\":%u,\"scale\":%u,\"cc\":%u}",rounding,fmz,scale,cc);
}
static void evaluate(const NvnDiscoveryRecipe *recipe,size_t index,unsigned slot,
                     const ShaderKey *key,bool context_available,const char *directory,bool hashes) {
    unsigned stage=recipe->rec.kind=='C'?STAGE_COMPUTE:slot?STAGE_FRAGMENT:STAGE_VERTEX;
    MaxwellSpirvOutput out={0};
    MaxwellSpirvStatus status=MAXWELL_SPIRV_UNSUPPORTED;
    if(context_available)status=maxwell_direct_spirv(recipe->code[slot],recipe->code_bytes[slot],
        recipe->constant_bytes[slot],(int)stage,key,&out);
    else snprintf(out.reason,sizeof out.reason,"vertex metadata unavailable for fragment context");
    bool saved=status==MAXWELL_SPIRV_OK&&save_words(directory,index,stage,&out);
    printf("{\"record\":%zu,\"stage\":%u,\"status\":%u,\"context_available\":%s,\"saved\":%s,\"reason\":",
        index,stage,status,context_available?"true":"false",saved?"true":"false");quoted(out.reason);
    printf(",\"word_count\":%zu",out.word_count);
    uint8_t identity[32],code_digest[32]={0},encoded[128];
    NvnDiscWriter writer={encoded,0,sizeof encoded,true};
    bool digest_ok=shared_sha256_bytes(recipe->code[slot],
        (size_t)recipe->code_bytes[slot]+recipe->constant_bytes[slot],code_digest);
    nvn_disc_w8(&writer,(uint8_t)stage);nvn_disc_w32(&writer,recipe->code_bytes[slot]);nvn_disc_w32(&writer,recipe->constant_bytes[slot]);
    nvn_disc_wbytes(&writer,code_digest,sizeof code_digest);nvn_disc_write_key(&writer,key);nvn_disc_w32(&writer,key->vs_out_mask);
    digest_ok=digest_ok&&writer.ok&&shared_sha256_bytes(encoded,writer.n,identity);
    if(digest_ok){printf(",\"input_identity\":\"");for(unsigned i=0;i<32;i++)printf("%02x",identity[i]);putchar('"');}
    if(status==MAXWELL_SPIRV_OK&&out.words&&shared_sha256_bytes(out.words,out.word_count*sizeof(uint32_t),identity)) {
        printf(",\"spirv_identity\":\"");for(unsigned i=0;i<32;i++)printf("%02x",identity[i]);putchar('"');
    }
    if(status!=MAXWELL_SPIRV_OK&&out.instruction_offset>=0x80&&out.instruction_offset<=recipe->code_bytes[slot]&&recipe->code_bytes[slot]-out.instruction_offset>=8) {
        uint64_t word;memcpy(&word,recipe->code[slot]+out.instruction_offset,8);
        const char *name=instruction_class(word);printf(",\"first_rejected_instruction\":");quoted(name);modifier_fields(word,name);
#ifdef DIRECT_SPIRV_COVERAGE_EDEN_OPCODES
        printf(",\"reference_instruction\":");quoted(reference_instruction(word));
#endif
    }
    if(hashes)printf(",\"code_hash\":\"%016" PRIx64 "\"",slot?recipe->rec.fs:recipe->rec.vs);
    puts("}");
    maxwell_direct_spirv_free(&out);
}
int main(int argc,char **argv) {
    if(argc!=4){fprintf(stderr,"usage: direct-spv-coverage JOURNAL PRIVATE_TEMP HASHES\n");return 2;}
    umask(0077);uint8_t header[NVN_DISCOVERY_HEADER_BYTES],identity[32];
    FILE *f=fopen(argv[1],"rb");if(!f){fprintf(stderr,"cannot open journal\n");return 1;}
    bool header_ok=fread(header,1,sizeof header,f)==sizeof header&&!ferror(f);if(fclose(f))header_ok=false;
    if(!header_ok||memcmp(header,nvn_disc_magic,8)){fprintf(stderr,"invalid journal header\n");return 1;}
    memcpy(identity,header+24,32);NvnDiscoveryStore store={0};
    NvnDiscoveryOpenResult opened=nvn_discovery_open(&store,argv[1],identity,shared_sha256_bytes,
        32u*1024u*1024u,NVN_DISCOVERY_MAX_RECORDS);
    if(opened!=NVN_DISCOVERY_OPEN_LOADED&&opened!=NVN_DISCOVERY_OPEN_RECOVERED){
        fprintf(stderr,"journal integrity/bounds validation failed: %d\n",opened);return 1;
    }
    printf("{\"journal_records\":%u,\"recovered_prefix\":%s}\n",store.count,opened==NVN_DISCOVERY_OPEN_RECOVERED?"true":"false");
    bool hashes=!strcmp(argv[3],"1");
    for(size_t i=0;i<store.count;i++) {
        NvnDiscoveryRecipe recipe={0};if(!nvn_discovery_read_at(&store,i,&recipe)){fprintf(stderr,"journal record validation failed\n");nvn_discovery_close(&store);return 1;}
        ShaderInfo *vertex=NULL;
        if(recipe.rec.kind=='G') {
            vertex=maxwell_translate_sized(recipe.code[0],recipe.code_bytes[0],recipe.constant_bytes[0],STAGE_VERTEX,&recipe.vkey);
            evaluate(&recipe,i,0,&recipe.vkey,true,argv[2],hashes);
            if(recipe.rec.fs){ShaderKey fragment=recipe.fkey;if(vertex)fragment.vs_out_mask=vertex->out_mask;
                evaluate(&recipe,i,1,&fragment,vertex&&vertex->msl,argv[2],hashes);}
        } else evaluate(&recipe,i,0,&recipe.vkey,true,argv[2],hashes);
        maxwell_free(vertex);nvn_discovery_recipe_release(&recipe);
    }
    nvn_discovery_close(&store);return 0;
}
