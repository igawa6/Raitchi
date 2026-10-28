#define _GNU_SOURCE
#include "continuous_image.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define IMAGE_MAX_MODULES 4u
#define IMAGE_MAX_CPUS 256u
#define IMAGE_CACHE_SLOTS 64u
#define IMAGE_BANK_SIZE (16u*1024u*1024u)
#define IMAGE_HLE_SLOTS 4096u
#define IMAGE_HLE_STRIDE 16u
typedef struct ExitRecord {uint64_t pc;const CnceSite *site;} ExitRecord;
typedef struct DynamicSlot {
    _Atomic uint64_t sequence,target;
    _Atomic uint32_t kind;
    _Atomic(const CnceSite*) site;
} DynamicSlot;
typedef struct ImageBank {
    CnceImageModule module;
    uint64_t text_lo,text_hi,text_pages_hi,lo,hi,exit_end,resume_lo,dynamic_lo;
    uint64_t *returns;size_t return_count;
    DynamicSlot dynamic[IMAGE_MAX_CPUS][IMAGE_CACHE_SLOTS];
    unsigned next[IMAGE_MAX_CPUS];
} ImageBank;
typedef struct ContinuousImage {
    SessionLayout layout;
    size_t page,module_count,site_count,site_mapping_size,exit_count;
    CnceSite *sites;ExitRecord *exits;
    ImageBank banks[IMAGE_MAX_MODULES];
    uint64_t hle_lo,hle_hi,hle_exit_lo,hle_resume_lo,return_target;
    unsigned char *hle_backup;
    _Atomic(Cpu*) cpus[IMAGE_MAX_CPUS];
    pthread_mutex_t lock;
    unsigned text_modified,hle_modified;
} ContinuousImage;
static _Atomic(ContinuousImage*) current_image;
static uint64_t align_up(uint64_t value,size_t page){return (value+page-1)&~(uint64_t)(page-1);}
static int range_overlap(uint64_t lo,uint64_t hi,uint64_t lo2,uint64_t hi2){return lo<hi2&&lo2<hi;}
static uint32_t get_word(uint64_t pc){uint32_t word;memcpy(&word,(const void*)(uintptr_t)pc,4);return word;}
static int compare_request(const void *a,const void *b){uint64_t x=((const CncePatchRequest*)a)->pc,y=((const CncePatchRequest*)b)->pc;return (x>y)-(x<y);}
static int compare_site(const void *a,const void *b){uint64_t x=((const CnceSite*)a)->original_pc,y=((const CnceSite*)b)->original_pc;return (x>y)-(x<y);}
static int compare_u64(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);}
static int compare_exit(const void *a,const void *b){uint64_t x=((const ExitRecord*)a)->pc,y=((const ExitRecord*)b)->pc;return (x>y)-(x<y);}
static void error_set(char *error,size_t size,const char *message){if(error&&size)snprintf(error,size,"%s",message);}
static ImageBank *bank_for(ContinuousImage *image,uint64_t pc){for(size_t i=0;i<image->module_count;i++)if(pc>=image->banks[i].text_lo&&pc<image->banks[i].text_hi)return &image->banks[i];return NULL;}
static const CnceSite *find_site(const ContinuousImage *image,uint64_t pc) {
    size_t lo=0,hi=image->site_count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(image->sites[mid].original_pc<pc)lo=mid+1;else hi=mid;}
    return lo<image->site_count&&image->sites[lo].original_pc==pc?&image->sites[lo]:NULL;
}
static int valid_site(const ContinuousImage *image,const CnceSite *site) {
    uintptr_t address=(uintptr_t)site,base=(uintptr_t)image->sites;
    return address>=base&&address-base<image->site_count*sizeof *site&&(address-base)%sizeof *site==0;
}
static int fake_hle(const ContinuousImage *image,uint64_t pc){return pc>=image->hle_lo&&pc-image->hle_lo<IMAGE_HLE_SLOTS*IMAGE_HLE_STRIDE&&(pc-image->hle_lo)%IMAGE_HLE_STRIDE==0;}
static int gap(ContinuousImage *image,size_t index,uint64_t lo,uint64_t hi,uint64_t *advance) {
    if(lo<image->layout.window.lo||hi>image->layout.window.hi||hi<=lo)return 0;
    *advance=lo;
    for(unsigned i=0;i<image->layout.module_count;i++) {
        SessionRange range=image->layout.modules[i].image;
        if(range_overlap(lo,hi,range.lo,range.hi)&&range.hi>*advance)*advance=range.hi;
    }
    for(unsigned i=0;i<SESSION_REGION_COUNT;i++) {
        SessionRange range=image->layout.regions[i];
        if(range_overlap(lo,hi,range.lo,range.hi)&&range.hi>*advance)*advance=range.hi;
    }
    for(size_t i=0;i<index;i++)if(range_overlap(lo,hi,image->banks[i].lo,image->banks[i].hi)&&image->banks[i].hi>*advance)*advance=image->banks[i].hi;
    return *advance==lo;
}
static int bank_reachable(const ImageBank *bank,uint64_t lo,uint64_t hi) {
    uint32_t branch;
    return cnce_encode_b26(bank->text_lo,lo,&branch)==CNCE_PATCH_OK&&
           cnce_encode_b26(bank->text_lo,hi-4,&branch)==CNCE_PATCH_OK&&
           cnce_encode_b26(bank->text_hi-4,lo,&branch)==CNCE_PATCH_OK&&
           cnce_encode_b26(bank->text_hi-4,hi-4,&branch)==CNCE_PATCH_OK;
}
static int choose_bank(ContinuousImage *image,size_t index,char *failure,size_t capacity) {
    ImageBank *bank=&image->banks[index];uint64_t advance=0;
    const char *before="outside",*after="outside";
    /* A before-image bank is usable only inside the already owned window.
     * In particular, never borrow a module's BSS or a fixed HLE region. */
    if(bank->module.base>=IMAGE_BANK_SIZE) {
        uint64_t lo=bank->module.base-IMAGE_BANK_SIZE,hi=bank->module.base;
        if(lo>=image->layout.window.lo&&hi<=image->layout.window.hi) {
            before="occupied";
            if(gap(image,index,lo,hi,&advance)) {
                before="B26";
                if(bank_reachable(bank,lo,hi)){bank->lo=lo;bank->hi=hi;return 1;}
            }
        }
    }
    uint64_t candidate=align_up(bank->module.base+bank->module.image_size,image->page);
    for(unsigned n=0;n<64;n++) {
        if(candidate>UINT64_MAX-IMAGE_BANK_SIZE||candidate>UINT64_MAX-image->page)break;
        uint64_t hi=candidate+IMAGE_BANK_SIZE;
        if(candidate<image->layout.window.lo||hi>image->layout.window.hi){after="outside";break;}
        after="occupied";
        if(gap(image,index,candidate,hi,&advance)) {
            if(!bank_reachable(bank,candidate,hi)){after="B26";break;}
            bank->lo=candidate;bank->hi=hi;return 1;
        }
        if(advance<=candidate||advance>UINT64_MAX-image->page)break;
        candidate=align_up(advance,image->page);
    }
    snprintf(failure,capacity,"module %zu has no branch-reachable trampoline bank: text +0x%llx+0x%llx; before [-0x%x,0) %s; after +0x%llx+0x%x %s",
             index,(unsigned long long)bank->module.text_offset,(unsigned long long)bank->module.text_size,
             IMAGE_BANK_SIZE,before,(unsigned long long)(candidate-bank->module.base),IMAGE_BANK_SIZE,after);
    return 0;
}
static void release_image(ContinuousImage *image,int restore) {
    if(!image)return;
    if(restore) {
        for(size_t m=0;m<image->module_count;m++) {
            ImageBank *bank=&image->banks[m];
            if(image->text_modified&&mprotect((void*)(uintptr_t)bank->text_lo,bank->text_pages_hi-bank->text_lo,PROT_READ|PROT_WRITE))abort();
            if(image->text_modified&&bank->hi&&mprotect((void*)(uintptr_t)bank->lo,bank->hi-bank->lo,PROT_READ|PROT_WRITE))abort();
        }
        if(image->text_modified)for(size_t i=0;i<image->site_count;i++)if(bank_for(image,image->sites[i].original_pc))
            memcpy((void*)(uintptr_t)image->sites[i].original_pc,&image->sites[i].original_word,4);
        if(image->hle_modified) {
            if(mprotect((void*)(uintptr_t)image->hle_lo,image->hle_hi-image->hle_lo,PROT_READ|PROT_WRITE))abort();
            memcpy((void*)(uintptr_t)image->hle_lo,image->hle_backup,image->hle_hi-image->hle_lo);
        }
    }
    if(image->sites)munmap(image->sites,image->site_mapping_size);
    for(size_t m=0;m<image->module_count;m++)free(image->banks[m].returns);
    free(image->exits);free(image->hle_backup);pthread_mutex_destroy(&image->lock);free(image);
}
static int initialize_returns(ContinuousImage *image,ImageBank *bank) {
    size_t count=1;
    for(uint64_t pc=bank->text_lo;pc+4<bank->text_hi;pc+=4) {
        uint32_t word=get_word(pc);
        if((word&0xfc000000u)==0x94000000u||(word&0xfffffc1fu)==0xd63f0000u)count++;
    }
    for(size_t i=0;i<image->site_count;i++)if(image->sites[i].original_pc>=bank->text_lo&&image->sites[i].resume_pc<bank->text_hi)count++;
    if(count>IMAGE_BANK_SIZE/8)return 0;
    bank->returns=malloc(count*sizeof *bank->returns);if(!bank->returns)return 0;
    count=0;bank->returns[count++]=bank->text_lo;
    for(uint64_t pc=bank->text_lo;pc+4<bank->text_hi;pc+=4) {
        uint32_t word=get_word(pc);
        if((word&0xfc000000u)==0x94000000u||(word&0xfffffc1fu)==0xd63f0000u)bank->returns[count++]=pc+4;
    }
    for(size_t i=0;i<image->site_count;i++)if(image->sites[i].original_pc>=bank->text_lo&&image->sites[i].resume_pc<bank->text_hi)bank->returns[count++]=image->sites[i].resume_pc;
    qsort(bank->returns,count,sizeof *bank->returns,compare_u64);size_t unique=0;
    for(size_t i=0;i<count;i++)if(!i||bank->returns[i]!=bank->returns[i-1])bank->returns[unique++]=bank->returns[i];
    bank->return_count=unique;return 1;
}
int continuous_image_prepare(const SessionLayout *layout,const CnceImageModule *modules,size_t module_count,
    const CncePatchRequest *requests,size_t request_count,uint64_t exit_gateway,char *error,size_t error_capacity) {
    if(atomic_load_explicit(&current_image,memory_order_acquire)){error_set(error,error_capacity,"image already prepared");return -1;}
    if(!layout||!modules||!module_count||module_count>IMAGE_MAX_MODULES||module_count!=layout->module_count||
       (!requests&&request_count)||!exit_gateway||(exit_gateway&3)||request_count>1000000){error_set(error,error_capacity,"invalid preparation arguments");return -1;}
    long page=sysconf(_SC_PAGESIZE);
    if(page<=0||((unsigned long)page&((unsigned long)page-1))||layout->struct_size!=sizeof *layout||layout->version!=SESSION_LAYOUT_VERSION||
       layout->page_size!=(uint32_t)page||layout->window.hi<=layout->window.lo||layout->window.hi>UINT64_MAX-(uint64_t)page||
       (layout->window.lo&((uint64_t)page-1))||(layout->window.hi&((uint64_t)page-1))){error_set(error,error_capacity,"invalid owned reservation");return -1;}
    for(unsigned r=0;r<SESSION_REGION_COUNT;r++) {
        SessionRange range=layout->regions[r];
        if(range.hi<=range.lo||range.lo<layout->window.lo||range.hi>layout->window.hi||
           (range.lo&((uint64_t)page-1))||(range.hi&((uint64_t)page-1))){error_set(error,error_capacity,"invalid owned region");return -1;}
        for(unsigned other=0;other<r;other++)if(range_overlap(range.lo,range.hi,layout->regions[other].lo,layout->regions[other].hi)){error_set(error,error_capacity,"overlapping owned regions");return -1;}
    }
    ContinuousImage *image=calloc(1,sizeof *image);CncePatchRequest *sorted=NULL;
    char failure[256]="continuous image validation, capacity or protection failed";
    if(!image){error_set(error,error_capacity,"metadata allocation failed");return -1;}
    pthread_mutex_init(&image->lock,NULL);image->layout=*layout;image->page=(size_t)page;image->module_count=module_count;
    if(request_count){sorted=malloc(request_count*sizeof *sorted);if(!sorted)goto failed;memcpy(sorted,requests,request_count*sizeof *sorted);qsort(sorted,request_count,sizeof *sorted,compare_request);}
    for(size_t i=0;i<request_count;i++)if((i&&sorted[i].pc==sorted[i-1].pc)||sorted[i].reserved||
       (sorted[i].kind!=CNCE_PATCH_HOST_FUNCTION&&sorted[i].kind!=CNCE_PATCH_INSTRUCTION_HOOK))goto failed;
    image->hle_lo=layout->regions[SESSION_HLE].lo;image->hle_hi=layout->regions[SESSION_HLE].hi;
    if((image->hle_lo&(image->page-1))||(image->hle_hi&(image->page-1))||image->hle_lo<layout->window.lo||image->hle_hi>layout->window.hi||
       image->hle_hi<=image->hle_lo||image->hle_hi-image->hle_lo<0x40000+image->page)goto failed;
    image->hle_exit_lo=image->hle_lo+IMAGE_HLE_SLOTS*IMAGE_HLE_STRIDE;
    image->hle_resume_lo=image->hle_exit_lo+IMAGE_HLE_SLOTS*CNCE_EXIT_STUB_SIZE;
    image->return_target=image->hle_hi-image->page;
    if(image->hle_resume_lo+IMAGE_HLE_SLOTS*CNCE_RESUME_STUB_SIZE>image->return_target)goto failed;
    size_t total=IMAGE_HLE_SLOTS+1;
    for(size_t m=0;m<module_count;m++) {
        const CnceImageModule *module=&modules[m];SessionRange selected=layout->modules[m].image;ImageBank *bank=&image->banks[m];
        if(module->base!=selected.lo||selected.hi<=selected.lo||module->image_size!=selected.hi-selected.lo||
           module->base<layout->window.lo||selected.hi>layout->window.hi||(module->base&(image->page-1))||
           (module->text_offset&(image->page-1))||!module->text_size||(module->text_size&3)||
           module->text_offset>module->image_size||module->text_size>module->image_size-module->text_offset)goto failed;
        bank->module=*module;bank->text_lo=module->base+module->text_offset;bank->text_hi=bank->text_lo+module->text_size;
        bank->text_pages_hi=align_up(bank->text_hi,image->page);if(bank->text_pages_hi>selected.hi)goto failed;
        for(size_t other=0;other<m;other++)if(range_overlap(selected.lo,selected.hi,layout->modules[other].image.lo,layout->modules[other].image.hi))goto failed;
        for(unsigned region=0;region<SESSION_REGION_COUNT;region++)if(range_overlap(selected.lo,selected.hi,layout->regions[region].lo,layout->regions[region].hi))goto failed;
        size_t start=0,n=0;while(start<request_count&&sorted[start].pc<bank->text_lo)start++;
        while(start+n<request_count&&sorted[start+n].pc<bank->text_hi)n++;
        size_t count=0;enum CncePatchStatus status=cnce_patch_collect((const void*)(uintptr_t)bank->text_lo,module->text_size,bank->text_lo,n?sorted+start:NULL,n,NULL,0,&count);
        if(status!=CNCE_PATCH_CAPACITY&&status!=CNCE_PATCH_OK) {
            uint64_t offset=0;
            if(status==CNCE_PATCH_UNSUPPORTED)for(;offset<module->text_size;offset+=4) {
                uint32_t kind;
                if(cnce_patch_classify(get_word(bank->text_lo+offset),&kind,NULL)<0)break;
            }
            snprintf(failure,sizeof failure,"module %zu patch collection: %s (text offset 0x%llx)",m,
                     cnce_patch_status_string(status),(unsigned long long)offset);
            goto failed;
        }
        if(count>1000000-total)goto failed;total+=count;
    }
    for(size_t i=0;i<request_count;i++)if(!bank_for(image,sorted[i].pc)&&!(fake_hle(image,sorted[i].pc)&&sorted[i].kind==CNCE_PATCH_HOST_FUNCTION))goto failed;
    image->site_mapping_size=(size_t)align_up(total*sizeof(CnceSite),image->page);
    image->sites=mmap(NULL,image->site_mapping_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(image->sites==MAP_FAILED){image->sites=NULL;goto failed;}
    for(size_t m=0;m<module_count;m++) {
        ImageBank *bank=&image->banks[m];size_t start=0,n=0,count=0;
        while(start<request_count&&sorted[start].pc<bank->text_lo)start++;
        while(start+n<request_count&&sorted[start+n].pc<bank->text_hi)n++;
        if(cnce_patch_collect((const void*)(uintptr_t)bank->text_lo,bank->module.text_size,bank->text_lo,n?sorted+start:NULL,n,image->sites+image->site_count,total-image->site_count,&count)!=CNCE_PATCH_OK)goto failed;
        image->site_count+=count;
    }
    for(unsigned i=0;i<IMAGE_HLE_SLOTS;i++)image->sites[image->site_count++]=(CnceSite){.original_pc=image->hle_lo+i*IMAGE_HLE_STRIDE,.resume_pc=image->hle_lo+i*IMAGE_HLE_STRIDE+4,.original_word=0xd503201f,.kind=CNCE_PATCH_HOST_FUNCTION};
    image->sites[image->site_count++]=(CnceSite){.original_pc=image->return_target,.resume_pc=image->return_target+4,.original_word=0xd503201f,.kind=CNCE_PATCH_HOST_FUNCTION};
    qsort(image->sites,image->site_count,sizeof *image->sites,compare_site);
    for(size_t i=0;i<request_count;i++)if(fake_hle(image,sorted[i].pc))((CnceSite*)find_site(image,sorted[i].pc))->payload=sorted[i].payload;
    free(sorted);sorted=NULL;
    size_t exits=0;for(size_t i=0;i<image->site_count;i++)if(image->sites[i].kind!=CNCE_PATCH_ORDERED_EXCLUSIVE)exits++;
    image->exits=calloc(exits,sizeof *image->exits);image->hle_backup=malloc(image->hle_hi-image->hle_lo);
    if(!image->exits||!image->hle_backup)goto failed;
    memcpy(image->hle_backup,(const void*)(uintptr_t)image->hle_lo,image->hle_hi-image->hle_lo);
    for(size_t m=0;m<module_count;m++) {
        ImageBank *bank=&image->banks[m];
        if(!choose_bank(image,m,failure,sizeof failure))goto failed;
        if(!initialize_returns(image,bank)){snprintf(failure,sizeof failure,"module %zu return inventory allocation/capacity failed",m);goto failed;}
        size_t n=0;for(size_t i=0;i<image->site_count;i++)if(image->sites[i].original_pc>=bank->text_lo&&image->sites[i].original_pc<bank->text_hi&&image->sites[i].kind!=CNCE_PATCH_ORDERED_EXCLUSIVE)n++;
        bank->exit_end=bank->lo+n*CNCE_EXIT_STUB_SIZE;bank->resume_lo=align_up(bank->exit_end,8);
        if(bank->resume_lo>bank->hi||bank->return_count>(bank->hi-bank->resume_lo)/8){snprintf(failure,sizeof failure,"module %zu trampoline exits/returns exceed bank capacity",m);goto failed;}
        bank->dynamic_lo=align_up(bank->resume_lo+bank->return_count*8,image->page);
        if(bank->dynamic_lo>bank->hi||IMAGE_MAX_CPUS*image->page>bank->hi-bank->dynamic_lo){snprintf(failure,sizeof failure,"module %zu dynamic cache pages exceed bank capacity",m);goto failed;}
    }
    /* All ownership/range/metadata checks precede any guest-memory mutation. */
    image->text_modified=1;image->hle_modified=1;
    uint32_t nop=0xd503201f;for(uint64_t pc=image->hle_lo;pc<image->hle_lo+IMAGE_HLE_SLOTS*IMAGE_HLE_STRIDE;pc+=4)memcpy((void*)(uintptr_t)pc,&nop,4);
    uint64_t next_exit[IMAGE_MAX_MODULES];for(size_t m=0;m<module_count;m++)next_exit[m]=image->banks[m].lo;
    for(size_t i=0;i<image->site_count;i++) {
        CnceSite *site=&image->sites[i];ImageBank *bank=bank_for(image,site->original_pc);uint64_t stub_pc=0;uint32_t replacement;
        if(site->kind!=CNCE_PATCH_ORDERED_EXCLUSIVE) {
            if(bank){size_t m=(size_t)(bank-image->banks);stub_pc=next_exit[m];next_exit[m]+=CNCE_EXIT_STUB_SIZE;}
            else if(site->original_pc==image->return_target)stub_pc=image->return_target+32;
            else stub_pc=image->hle_exit_lo+(site->original_pc-image->hle_lo)/IMAGE_HLE_STRIDE*CNCE_EXIT_STUB_SIZE;
        }
        if(cnce_patch_emit(site,stub_pc,(uintptr_t)site,exit_gateway,&replacement,(void*)(uintptr_t)stub_pc,CNCE_EXIT_STUB_SIZE)!=CNCE_PATCH_OK)goto failed;
        memcpy((void*)(uintptr_t)site->original_pc,&replacement,4);
        if(stub_pc)image->exits[image->exit_count++]=(ExitRecord){stub_pc,site};
    }
    for(unsigned i=0;i<IMAGE_HLE_SLOTS;i++)if(cnce_resume_emit(image->hle_resume_lo+i*8,image->hle_lo+i*IMAGE_HLE_STRIDE,(void*)(uintptr_t)(image->hle_resume_lo+i*8),8)!=CNCE_PATCH_OK)goto failed;
    for(size_t m=0;m<module_count;m++) {
        ImageBank *bank=&image->banks[m];
        for(size_t i=0;i<bank->return_count;i++)if(cnce_resume_emit(bank->resume_lo+i*8,bank->returns[i],(void*)(uintptr_t)(bank->resume_lo+i*8),8)!=CNCE_PATCH_OK)goto failed;
        __builtin___clear_cache((char*)(uintptr_t)bank->text_lo,(char*)(uintptr_t)bank->text_pages_hi);
        __builtin___clear_cache((char*)(uintptr_t)bank->lo,(char*)(uintptr_t)bank->dynamic_lo);
        if(mprotect((void*)(uintptr_t)bank->lo,bank->dynamic_lo-bank->lo,PROT_READ|PROT_EXEC)||mprotect((void*)(uintptr_t)bank->text_lo,bank->text_pages_hi-bank->text_lo,PROT_READ|PROT_EXEC))goto failed;
    }
    __builtin___clear_cache((char*)(uintptr_t)image->hle_lo,(char*)(uintptr_t)image->hle_hi);
    if(mprotect((void*)(uintptr_t)image->hle_lo,image->hle_hi-image->hle_lo,PROT_READ|PROT_EXEC)||mprotect(image->sites,image->site_mapping_size,PROT_READ))goto failed;
    qsort(image->exits,image->exit_count,sizeof *image->exits,compare_exit);
    atomic_store_explicit(&current_image,image,memory_order_release);error_set(error,error_capacity,"");return 0;
failed:
    free(sorted);release_image(image,1);error_set(error,error_capacity,failure);return -1;
}
static int safe_displaced_word(uint32_t word) {
    uint32_t kind;int classified=cnce_patch_classify(word,&kind,NULL);
    if(classified<0||(classified&&kind!=CNCE_PATCH_ORDERED_EXCLUSIVE))return 0;
    if((word&0x7c000000u)==0x14000000u|| /* B/BL */
       (word&0xff000010u)==0x54000000u|| /* B.cond */
       (word&0x7e000000u)==0x34000000u|| /* CBZ/NZ */
       (word&0x7e000000u)==0x36000000u|| /* TBZ/NZ */
       (word&0xfe000000u)==0xd6000000u|| /* register branch/return */
       (word&0x1f000000u)==0x10000000u|| /* ADR/ADRP */
       (word&0x3b000000u)==0x18000000u|| /* literal load/PRFM */
       (word&0xff000000u)==0xd4000000u)return 0;
    return 1;
}
static const void *private_stub(ContinuousImage *image,ImageBank *bank,Cpu *cpu,
    uint64_t target,const CnceSite *site,uint32_t kind) {
    if(!cpu||((uintptr_t)cpu&15))return NULL;
    pthread_mutex_lock(&image->lock);unsigned owner=IMAGE_MAX_CPUS;
    for(unsigned i=0;i<IMAGE_MAX_CPUS;i++)if(atomic_load_explicit(&image->cpus[i],memory_order_relaxed)==cpu){owner=i;break;}
    if(owner==IMAGE_MAX_CPUS)for(unsigned i=0;i<IMAGE_MAX_CPUS;i++)if(!atomic_load_explicit(&image->cpus[i],memory_order_relaxed)) {
        owner=i;atomic_store_explicit(&image->cpus[i],cpu,memory_order_release);break;
    }
    if(owner==IMAGE_MAX_CPUS){pthread_mutex_unlock(&image->lock);return NULL;}
    for(unsigned i=0;i<IMAGE_CACHE_SLOTS;i++) {
        DynamicSlot *slot=&bank->dynamic[owner][i];
        if(atomic_load_explicit(&slot->sequence,memory_order_relaxed)&&
           atomic_load_explicit(&slot->target,memory_order_relaxed)==target&&
           atomic_load_explicit(&slot->kind,memory_order_relaxed)==kind&&
           atomic_load_explicit(&slot->site,memory_order_relaxed)==site) {
            const void *result=(const void*)(uintptr_t)(bank->dynamic_lo+owner*image->page+i*16);
            pthread_mutex_unlock(&image->lock);return result;
        }
    }
    unsigned index=bank->next[owner]++%IMAGE_CACHE_SLOTS;uint64_t pc=bank->dynamic_lo+owner*image->page+index*16;
    uint64_t page=bank->dynamic_lo+owner*image->page;unsigned char code[16]={0};
    enum CncePatchStatus status;
    if(kind==CNCE_IMAGE_PC_RESUME)status=cnce_resume_emit(pc,target,code,sizeof code);
    else {
        uint32_t restore=0xa8c147f0,branch=0,word=site->original_word;
        if(site->original_kind==CNCE_PATCH_ORDERED_EXCLUSIVE)word|=0x8000u;
        status=cnce_encode_b26(pc+8,site->resume_pc,&branch);
        memcpy(code,&restore,4);memcpy(code+4,&word,4);memcpy(code+8,&branch,4);
    }
    if(status!=CNCE_PATCH_OK||mprotect((void*)(uintptr_t)page,image->page,PROT_READ|PROT_WRITE)){
        pthread_mutex_unlock(&image->lock);return NULL;
    }
    DynamicSlot *slot=&bank->dynamic[owner][index];
    atomic_fetch_add_explicit(&slot->sequence,1,memory_order_acq_rel); /* odd: unpublished */
    memcpy((void*)(uintptr_t)pc,code,sizeof code);
    atomic_store_explicit(&slot->target,target,memory_order_relaxed);
    atomic_store_explicit(&slot->kind,kind,memory_order_relaxed);
    atomic_store_explicit(&slot->site,site,memory_order_relaxed);
    __builtin___clear_cache((char*)(uintptr_t)pc,(char*)(uintptr_t)(pc+sizeof code));
    if(mprotect((void*)(uintptr_t)page,image->page,PROT_READ|PROT_EXEC))abort();
    atomic_fetch_add_explicit(&slot->sequence,1,memory_order_release); /* even: RX ready */
    pthread_mutex_unlock(&image->lock);return (const void*)(uintptr_t)pc;
}
const void *continuous_image_resume(Cpu *cpu,uint64_t target) {
    ContinuousImage *image=atomic_load_explicit(&current_image,memory_order_acquire);
    if(!image||(target&3)||!cpu||((uintptr_t)cpu&15))return NULL;
    if(fake_hle(image,target))return (const void*)(uintptr_t)(image->hle_resume_lo+(target-image->hle_lo)/IMAGE_HLE_STRIDE*8);
    ImageBank *bank=bank_for(image,target);if(!bank)return NULL;
    size_t lo=0,hi=bank->return_count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(bank->returns[mid]<target)lo=mid+1;else hi=mid;}
    if(lo<bank->return_count&&bank->returns[lo]==target)return (const void*)(uintptr_t)(bank->resume_lo+lo*8);
    return private_stub(image,bank,cpu,target,NULL,CNCE_IMAGE_PC_RESUME);
}
const void *continuous_image_escape(Cpu *cpu,const CnceSite *site) {
    ContinuousImage *image=atomic_load_explicit(&current_image,memory_order_acquire);
    if(!image||!valid_site(image,site)||!safe_displaced_word(site->original_word))return NULL;
    ImageBank *bank=bank_for(image,site->original_pc);if(!bank)return NULL;
    return private_stub(image,bank,cpu,site->original_pc,site,CNCE_IMAGE_PC_ESCAPE);
}
uint64_t continuous_image_return_target(void){ContinuousImage *image=atomic_load_explicit(&current_image,memory_order_acquire);return image?image->return_target:0;}
const CnceSite *continuous_image_site_at(uint64_t pc){ContinuousImage *image=atomic_load_explicit(&current_image,memory_order_acquire);return image?find_site(image,pc):NULL;}
int continuous_image_valid_event(const CnceSite *site){ContinuousImage *image=atomic_load_explicit(&current_image,memory_order_acquire);return image&&valid_site(image,site)&&site->kind!=CNCE_PATCH_ORDERED_EXCLUSIVE;}
int continuous_image_is_original_guest_pc(uint64_t pc){ContinuousImage *image=atomic_load_explicit(&current_image,memory_order_acquire);return image&&!(pc&3)&&bank_for(image,pc)!=NULL;}
static int dynamic_info(const ImageBank *bank,unsigned owner,unsigned index,uint64_t pc,CnceImagePcInfo *info) {
    const DynamicSlot *slot=&bank->dynamic[owner][index];uint64_t sequence=atomic_load_explicit(&slot->sequence,memory_order_acquire);
    if(!sequence||(sequence&1))return 0;
    uint64_t target=atomic_load_explicit(&slot->target,memory_order_relaxed);uint32_t kind=atomic_load_explicit(&slot->kind,memory_order_relaxed);
    const CnceSite *site=atomic_load_explicit(&slot->site,memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if(sequence!=atomic_load_explicit(&slot->sequence,memory_order_acquire))return 0;
    unsigned instruction=(unsigned)((pc&15)/4);
    if(instruction>=(kind==CNCE_IMAGE_PC_ESCAPE?3u:2u))return 0;
    *info=(CnceImagePcInfo){.original_pc=kind==CNCE_IMAGE_PC_ESCAPE&&instruction==2?site->resume_pc:target,.site=site,.kind=kind,.instruction_index=instruction};return 1;
}
int continuous_image_pc_info(uint64_t pc,CnceImagePcInfo *info) {
    ContinuousImage *image=atomic_load_explicit(&current_image,memory_order_acquire);
    if(!image||!info||(pc&3))return 0;
    if(bank_for(image,pc)){*info=(CnceImagePcInfo){.original_pc=pc,.site=find_site(image,pc),.kind=CNCE_IMAGE_PC_ORIGINAL};return 1;}
    if(fake_hle(image,pc)||pc==image->return_target){*info=(CnceImagePcInfo){.original_pc=pc,.site=find_site(image,pc),.kind=CNCE_IMAGE_PC_ORIGINAL};return 1;}
    size_t lo=0,hi=image->exit_count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(image->exits[mid].pc<=pc)lo=mid+1;else hi=mid;}
    if(lo&&pc-image->exits[lo-1].pc<16) {
        const ExitRecord *record=&image->exits[lo-1];*info=(CnceImagePcInfo){.original_pc=record->site->original_pc,.site=record->site,.kind=CNCE_IMAGE_PC_EXIT,.instruction_index=(uint32_t)((pc-record->pc)/4)};return 1;
    }
    if(pc>=image->hle_resume_lo&&pc<image->hle_resume_lo+IMAGE_HLE_SLOTS*8){*info=(CnceImagePcInfo){.original_pc=image->hle_lo+(pc-image->hle_resume_lo)/8*IMAGE_HLE_STRIDE,.kind=CNCE_IMAGE_PC_RESUME,.instruction_index=(uint32_t)((pc-image->hle_resume_lo)%8/4)};return 1;}
    for(size_t m=0;m<image->module_count;m++) {
        ImageBank *bank=&image->banks[m];
        if(pc>=bank->resume_lo&&pc<bank->resume_lo+bank->return_count*8){*info=(CnceImagePcInfo){.original_pc=bank->returns[(pc-bank->resume_lo)/8],.kind=CNCE_IMAGE_PC_RESUME,.instruction_index=(uint32_t)((pc-bank->resume_lo)%8/4)};return 1;}
        if(pc>=bank->dynamic_lo&&pc-bank->dynamic_lo<IMAGE_MAX_CPUS*image->page) {
            uint64_t offset=pc-bank->dynamic_lo;unsigned owner=(unsigned)(offset/image->page),index=(unsigned)(offset%image->page/16);
            if(index<IMAGE_CACHE_SLOTS)return dynamic_info(bank,owner,index,pc,info);
        }
    }
    return 0;
}
uint64_t continuous_image_original_pc(uint64_t pc){CnceImagePcInfo info;return continuous_image_pc_info(pc,&info)?info.original_pc:0;}
void continuous_image_finish_after_join(void){ContinuousImage *image=atomic_exchange_explicit(&current_image,NULL,memory_order_acq_rel);release_image(image,1);}
