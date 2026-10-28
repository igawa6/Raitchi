/* Asset-free numeric oracle fixtures, actual bounded parser and arithmetic. */
#include "services/audren_work_size.h"
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line%d: %s\n",__LINE__,#x);abort(); } } while (0)
typedef struct {const char *name;uint8_t raw[52];uint64_t expected;enum AudrenWorkSizeResult status;} Fixture;
#include "audren_work_size_fixtures.inc"
#include "ipc.h"
static void quiet_log(const char *format,...) { (void)format; }
#define LOG(...) quiet_log(__VA_ARGS__)
#include "audren_work_service.inc"
static void store(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void result(const uint8_t *p,size_t length,enum AudrenWorkSizeResult status,uint64_t expected) {
    struct {uint64_t before,value,after;} out={UINT64_C(0x1234567890abcdef),123,UINT64_C(0xfedcba0987654321)};
    CHECK(audren_work_size(p,length,&out.value)==status);CHECK(out.value==expected);
    CHECK(out.before==UINT64_C(0x1234567890abcdef)&&out.after==UINT64_C(0xfedcba0987654321));
}
static void fixtures_test(void) {
    for(unsigned f=0;f<sizeof fixtures/sizeof fixtures[0];f++) {
        const Fixture *fixture=&fixtures[f];uint8_t before[52];memcpy(before,fixture->raw,52);
        result(fixture->raw,52,fixture->status,fixture->expected);CHECK(!memcmp(fixture->raw,before,52));
        for(unsigned offset=1;offset<=7;offset++) {
            uint8_t unaligned[64];memset(unaligned,0x97,sizeof unaligned);memcpy(unaligned+offset,fixture->raw,52);
            result(unaligned+offset,52,fixture->status,fixture->expected);CHECK(!memcmp(unaligned+offset,fixture->raw,52));
            for(unsigned j=0;j<offset;j++)CHECK(unaligned[j]==0x97);
            for(unsigned j=offset+52;j<64;j++)CHECK(unaligned[j]==0x97);
        }
        if(fixture->status==AUDREN_WORK_SIZE_OK)CHECK(fixture->expected>0 && !(fixture->expected&4095));
    }
    CHECK(fixtures[0].expected==UINT64_C(0x44e000));
}
static void malformed_test(void) {
    uint8_t p[64];memset(p,0x17,sizeof p);memcpy(p,fixtures[0].raw,52);
    for(size_t n=0;n<52;n++)result(p,n,AUDREN_WORK_SIZE_ARGUMENT,0);
    for(size_t n=53;n<=64;n++)result(p,n,AUDREN_WORK_SIZE_ARGUMENT,0);
    result(NULL,52,AUDREN_WORK_SIZE_ARGUMENT,0);CHECK(audren_work_size(p,52,NULL)==AUDREN_WORK_SIZE_ARGUMENT);
    for(unsigned at=48;at<51;at++)for(unsigned bit=0;bit<8;bit++) {
        p[at]^=(uint8_t)(1u<<bit);result(p,52,AUDREN_WORK_SIZE_REVISION,0);p[at]^=(uint8_t)(1u<<bit);
    }
    for(unsigned byte=0;byte<256;byte++)if(byte<'1'||byte>';') {p[51]=(uint8_t)byte;result(p,52,AUDREN_WORK_SIZE_REVISION,0);}
    memcpy(p,fixtures[0].raw,52);
    const unsigned signed_counts[]={4,8,12,16,20,24,28,36,40};
    for(unsigned i=0;i<sizeof signed_counts/sizeof signed_counts[0];i++) {
        unsigned at=signed_counts[i];store(p+at,0x80000000);result(p,52,AUDREN_WORK_SIZE_PARAMETER,0);
        store(p+at,UINT32_MAX);result(p,52,AUDREN_WORK_SIZE_PARAMETER,0);memcpy(p,fixtures[0].raw,52);
    }
    // These bytes never participate in work-size arithmetic; ignoring them is
    // not a claim that an arbitrary configuration can initialize successfully.
    const unsigned irrelevant[]={0,1,2,3,32,33,34,35,44,45,46,47};
    for(unsigned i=0;i<sizeof irrelevant/sizeof irrelevant[0];i++) {
        p[irrelevant[i]]^=0xff;result(p,52,AUDREN_WORK_SIZE_OK,fixtures[0].expected);p[irrelevant[i]]^=0xff;
    }
}
static void zero(uint8_t p[52]) {memset(p,0,52);memcpy(p+48,"REV;",4);}
static void overflow_test(void) {
    uint8_t p[52];zero(p);store(p+4,INT32_MAX);store(p+8,INT32_MAX);store(p+20,INT32_MAX);
    result(p,52,AUDREN_WORK_SIZE_OVERFLOW,0); // sample-buffer product
    zero(p);store(p+12,46340);result(p,52,AUDREN_WORK_SIZE_OVERFLOW,0); // signed graph n*n
    zero(p);store(p+16,INT32_MAX);store(p+20,1);result(p,52,AUDREN_WORK_SIZE_OVERFLOW,0); // entry sum
    zero(p);store(p+16,1000000000);store(p+28,INT32_MAX);result(p,52,AUDREN_WORK_SIZE_OVERFLOW,0); // performance product
    zero(p);store(p+16,1000000000);store(p+36,1000000000);result(p,52,AUDREN_WORK_SIZE_OVERFLOW,0); // routed command product
    zero(p);uint64_t value;CHECK(audren_work_size(p,52,&value)==AUDREN_WORK_SIZE_OK);CHECK(value>0 && !(value&4095));
}
static void service_case(uint8_t *p,unsigned length,uint32_t expected_result,uint64_t expected_size) {
    struct {uint64_t before;IpcResp response;uint64_t after;} guarded;
    memset(&guarded,0x39,sizeof guarded);guarded.response=(IpcResp){0};memset(guarded.response.data,0xcc,sizeof guarded.response.data);
    IpcReq request={.cmd=1,.data=p,.data_size=length};work_size_service(&request,&guarded.response);
    CHECK(guarded.response.result==expected_result);CHECK(guarded.response.data_size==(expected_result?0:8));
    unsigned written=expected_result?0:8;
    if(written){uint64_t result;memcpy(&result,guarded.response.data,8);CHECK(result==expected_size);}
    for(unsigned i=written;i<sizeof guarded.response.data;i++)CHECK(guarded.response.data[i]==0xcc);
    CHECK(guarded.before==UINT64_C(0x3939393939393939)&&guarded.after==guarded.before);
    CHECK(!guarded.response.ncopy && !guarded.response.nmove && !guarded.response.nobjects);
}
static void service_test(void) {
    uint8_t p[64];memcpy(p,fixtures[0].raw,52);memset(p+52,0x87,12);
    service_case(p,52,0,UINT64_C(0x44e000));CHECK(!memcmp(p,fixtures[0].raw,52));
    service_case(p,51,0x70899,0);service_case(NULL,52,0x70899,0);service_case(p,53,0x70899,0);
    p[51]='<';service_case(p,52,0xc0299,0);p[51]=';';store(p+40,UINT32_MAX);service_case(p,52,0x70899,0);
}
int main(void) {
    fixtures_test();malformed_test();overflow_test();service_test();
    printf("state=passed checks=%u fixtures=%zu; actual52B parser/SDK-size arithmetic and real cmd1 case, no renderer allocation/execution\n",checks,sizeof fixtures/sizeof fixtures[0]);return 0;
}
