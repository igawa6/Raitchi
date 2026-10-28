/* Conservative AArch64 native-block planning. No game data or execution. */
#ifndef NATIVE_GAME_BLOCK_PLAN_H
#define NATIVE_GAME_BLOCK_PLAN_H
#include <stddef.h>
#include <stdint.h>

#define NGB_PLAN_VERSION 1u
#define NGB_MAX_NATIVE_WORDS 64u
/* A dynamic native context may use any unused GPR in this set. X16 is used by
 * the entry gateway; X18 belongs to Android; X19 is the guest register this
 * mode makes available. SP/XZR never occupies a context bit. */
#define NGB_CONTEXT_GPR_MASK (UINT32_C(0x7fffffff) & ~((UINT32_C(1)<<16)|(UINT32_C(1)<<18)|(UINT32_C(1)<<19)))
enum NgbStatus { NGB_OK, NGB_ARGUMENT, NGB_DECODER_ERROR, NGB_INVALID_INSTRUCTION };
enum NgbExitKind {
    NGB_EXIT_LIMIT, NGB_EXIT_INPUT_END, NGB_EXIT_BRANCH, NGB_EXIT_CALL,
    NGB_EXIT_RETURN, NGB_EXIT_SVC, NGB_EXIT_TLS, NGB_EXIT_PC_RELATIVE,
    NGB_EXIT_RESERVED_REGISTER, NGB_EXIT_ATOMIC, NGB_EXIT_SYSTEM,
    NGB_EXIT_EMULATE, NGB_EXIT_INVALID
};
enum NgbInstructionFlags {
    NGB_READ_SP=1u, NGB_WRITE_SP=2u, NGB_READ_NZCV=4u, NGB_WRITE_NZCV=8u,
    NGB_EXCLUSIVE_LOAD=16u, NGB_EXCLUSIVE_STORE=32u, NGB_CLEAR_EXCLUSIVE=64u,
    NGB_MEMORY=128u, NGB_VIRTUAL_REGISTER=256u
};
typedef struct NgbDecoder { uintptr_t handle; void *instruction; } NgbDecoder;
typedef struct NgbInstruction {
    uint64_t pc, target;
    uint32_t word, id, read_gpr, write_gpr, read_vector, write_vector, flags;
    enum NgbExitKind kind;
} NgbInstruction;
typedef struct NgbBlockPlan {
    uint32_t struct_size, version, native_count, gpr_usage;
    uint64_t start_pc, next_pc;
    uint32_t words[NGB_MAX_NATIVE_WORDS];
    NgbInstruction exit;
} NgbBlockPlan;

/* Each executing host thread owns its decoder. Capstone 5.0 detail/register
 * metadata is required; an unavailable/DIET decoder fails closed. */
enum NgbStatus ngb_decoder_init(NgbDecoder *decoder);
void ngb_decoder_destroy(NgbDecoder *decoder);
enum NgbStatus ngb_classify(NgbDecoder *decoder, const void *word,
                           uint64_t guest_pc, NgbInstruction *instruction);
/* Copy only the reviewed contiguous native-safe prefix. Exit instruction is
 * NEVER copied or executed; next_pc is its original guest PC. Limits/input-end
 * exits have no instruction. Caller initializes struct_size/version.
 * No memory mapping, rewriting, guest execution, or instruction emulation.
 * An exclusive window must remain entirely in the emulated engine until the
 * exclusive store/CLREX; the planner does not establish monitor continuity. */
enum NgbStatus ngb_plan_block(NgbDecoder *decoder, const void *code, size_t size,
                             uint64_t guest_pc, unsigned limit, NgbBlockPlan *plan);
/* Optional dynamic-context mode: original ordinary X19 instructions are
 * admitted, while X18 still exits. gpr_usage unions every body GPR read/write,
 * including aliases, memory bases/indexes and writeback. The prefix ends before
 * an instruction would consume its final unused NGB_CONTEXT_GPR_MASK register.
 * The executor must choose such an unused register, load real guest X19 and
 * preserve the chosen register's original guest value during enter/capture. */
enum NgbStatus ngb_plan_block_context(NgbDecoder *decoder, const void *code, size_t size,
                                     uint64_t guest_pc, unsigned limit, NgbBlockPlan *plan);
const char *ngb_status_string(enum NgbStatus status);
const char *ngb_exit_string(enum NgbExitKind kind);
#endif
