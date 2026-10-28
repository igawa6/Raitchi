/* Fixed-address continuous native code ownership. Configure before threads. */
#pragma once
#include "continuous-nce/patch_plan.h"
#include "runtime/aot_cpu.h"
#include "runtime/session_layout.h"
typedef struct CnceImageModule {
    uint64_t base,image_size,text_offset,text_size;
} CnceImageModule;
enum CnceImagePcKind {CNCE_IMAGE_PC_NONE,CNCE_IMAGE_PC_ORIGINAL,
    CNCE_IMAGE_PC_EXIT,CNCE_IMAGE_PC_RESUME,CNCE_IMAGE_PC_ESCAPE};
typedef struct CnceImagePcInfo {
    uint64_t original_pc;
    const CnceSite *site;
    uint32_t kind,instruction_index;
} CnceImagePcInfo;
/* layout must describe the already owned RW reservation, not an unreserved
 * address proposal. Module images must match its immutable module ranges.
 * Explicit requests may be unsorted; duplicates reject. New code pages use
 * only checked unoccupied gaps in this window, never MAP_FIXED/remapping.
 * Descriptors become R, text/code RX, never RWX. Failure restores text words
 * and RW permissions before releasing process-owned metadata. */
int continuous_image_prepare(const SessionLayout *layout,
    const CnceImageModule *modules,size_t module_count,
    const CncePatchRequest *requests,size_t request_count,uint64_t exit_gateway,
    char *error,size_t error_capacity);
/* Each actual Cpu owns its dedicated cache pages. Call only while that Cpu is
 * idle in host code. Nested invocations on one Cpu are serialized by its real
 * host thread. No returned trampoline may be executing during another call
 * for the same Cpu. Pages and metadata live until finish after every join. */
const void *continuous_image_resume(Cpu *cpu,uint64_t target);
/* Displaced ordinary instruction: restore16/17; originalword; B original+4.
 * PC-relative/control/virtual-system instructions reject. Caller must emulate
 * those using original metadata, then resume at the resulting original PC. */
const void *continuous_image_escape(Cpu *cpu,const CnceSite *site);
uint64_t continuous_image_return_target(void); /* dedicated HLE-tail sentinel */
const CnceSite *continuous_image_site_at(uint64_t original_pc);
int continuous_image_valid_event(const CnceSite *site);
int continuous_image_is_original_guest_pc(uint64_t pc); /* module text only */
/* Lock-free signal lookup. Explicit phases describe guest16/17 stack spills:
 * EXIT index0 beforeSTP; RESUME/ESCAPE index0 beforeLDP; ESCAPE index1 before
 * originalword, index2 after it. It returns0 for literal pools or unpublished
 * dynamic slots. Consumers must honor these phases when materializing Cpu. */
int continuous_image_pc_info(uint64_t native_pc,CnceImagePcInfo *info);
uint64_t continuous_image_original_pc(uint64_t native_pc); /* 0 if unknown */
void continuous_image_finish_after_join(void);
