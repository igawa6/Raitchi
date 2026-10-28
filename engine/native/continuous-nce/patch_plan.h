/* Continuous AArch64 patch metadata and own ISA encoders; no guest assets. */
#ifndef CONTINUOUS_NCE_PATCH_PLAN_H
#define CONTINUOUS_NCE_PATCH_PLAN_H
#include <stddef.h>
#include <stdint.h>

enum CncePatchKind {
    CNCE_PATCH_NONE=0, CNCE_PATCH_SVC=1, CNCE_PATCH_TPIDR_READ=2,
    CNCE_PATCH_TPIDRRO_READ=3, CNCE_PATCH_TPIDR_WRITE=4,
    CNCE_PATCH_COUNTER_READ=5, CNCE_PATCH_FREQUENCY_READ=6,
    CNCE_PATCH_ORDERED_EXCLUSIVE=7, CNCE_PATCH_HOST_FUNCTION=8,
    CNCE_PATCH_INSTRUCTION_HOOK=9
};
enum CncePatchStatus {
    CNCE_PATCH_OK=0, CNCE_PATCH_ARGUMENT, CNCE_PATCH_CAPACITY,
    CNCE_PATCH_RANGE, CNCE_PATCH_DUPLICATE, CNCE_PATCH_UNSUPPORTED
};
typedef struct CnceSite {
    uint64_t original_pc, resume_pc;
    uint32_t original_word, kind, operand, original_kind;
    uint64_t payload;
} CnceSite;
typedef struct CncePatchRequest {
    uint64_t pc;
    uint32_t kind, reserved; /* HOST_FUNCTION or INSTRUCTION_HOOK only */
    uint64_t payload;       /* opaque integration-owned identifier */
} CncePatchRequest;
#define CNCE_EXIT_STUB_SIZE 32u
#define CNCE_RESUME_STUB_SIZE 8u

/* Scalar classifier: returns1 for a patch,0 for an unchanged ordinary word,-1
 * for recognized unsupported privileged/system state. operand is GPR index for
 * TLS/counter or the SVC immediate; ordering replacement is original|bit15.
 * FPCR/FPSR/NZCV, ordinary branches/calls and memory remain hardware-native. */
int cnce_patch_classify(uint32_t word,uint32_t *kind,uint32_t *operand);
/* text size/base must be4aligned, no address wrap. Requests must be sorted by
 * strictly increasing PC inside text. One explicit request replaces an auto
 * patch, retaining original_kind; duplicates reject. required is set after
 * successful validation even on CAPACITY. Sites are untouched on failure.
 * No mapping, protection, writes to original text, allocation or execution. */
enum CncePatchStatus cnce_patch_collect(const void *text,size_t size,uint64_t base,
    const CncePatchRequest *requests,size_t request_count,CnceSite *sites,
    size_t capacity,size_t *required);
/* Exact signed B26 range[-128MiB,+128MiB-4]. Outputs unchanged on failure. */
enum CncePatchStatus cnce_encode_b26(uint64_t from,uint64_t to,uint32_t *word);
/* Site -> near32B exit stub. Stub saves guest16/17 onguestSP, loads immutable
 * descriptor address and absolute gateway, BRx17. Neither flags,LR,FP norother
 * GP registers change. descriptor must point to this site's immutable40B ABI.
 * ORDERED_EXCLUSIVE emits only replacementword and leaves stub bytes alone.
 * Caller reserves fresh writable storage, then publishes RX+flushes I-cache
 * while all guest execution is quiescent. Stub/outputs must not alias inputs. */
enum CncePatchStatus cnce_patch_emit(const CnceSite *site,uint64_t stub_pc,
    uint64_t descriptor_pc,uint64_t gateway_pc,uint32_t *replacement,
    void *stub,size_t stub_capacity);
/* Near-target resume gateway: LDPx16,x17,[SP],#16; Btarget. Entry must already
 * have restored guestSP-16 with saved16/17; preserves all otherstate. */
enum CncePatchStatus cnce_resume_emit(uint64_t stub_pc,uint64_t target_pc,
    void *stub,size_t capacity);
const char *cnce_patch_status_string(enum CncePatchStatus status);
#endif
