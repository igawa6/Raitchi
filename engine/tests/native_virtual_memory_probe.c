/* Generic instructions only. Actual Unicorn + actual host backing;
 * no game files, production KThread or runtime services are needed. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct Result {
    int error, model, completed, host_alias_correct, virtual_registers_correct;
    uint64_t pc, x0;
} Result;

static Result exercise(void *code, uint64_t *data, size_t page, int virtual_tlb) {
    Result result = {.error = UC_ERR_OK, .model = -1};
    uc_engine *uc = NULL;
    uc_err error = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc);
    if (error == UC_ERR_OK) error = uc_ctl_set_cpu_model(uc, UC_CPU_ARM64_A72);
    if (error == UC_ERR_OK) error = uc_ctl_set_tcg_buffer_size(uc, UINT32_C(2) << 20);
    if (error == UC_ERR_OK) error = uc_ctl_get_cpu_model(uc, &result.model); /* lazy init */
    if (error == UC_ERR_OK && virtual_tlb) error = uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL);
    if (error == UC_ERR_OK) error = uc_mem_map_ptr(uc, 0x1000, page, UC_PROT_READ | UC_PROT_EXEC, code);
    if (error == UC_ERR_OK) error = uc_mem_map_ptr(uc, (uintptr_t)data, page, UC_PROT_READ | UC_PROT_WRITE, data);
    uint64_t x18 = (uintptr_t)data, x19 = (uintptr_t)(data + 1);
    data[0] = UINT64_C(0x1020304050607080); data[1] = 0;
    if (error == UC_ERR_OK) error = uc_reg_write(uc, UC_ARM64_REG_X18, &x18);
    if (error == UC_ERR_OK) error = uc_reg_write(uc, UC_ARM64_REG_X19, &x19);
    if (error == UC_ERR_OK) error = uc_emu_start(uc, 0x1000, 0x100c, 0, 3);
    result.error = error;
    if (uc) {
        (void)uc_reg_read(uc, UC_ARM64_REG_PC, &result.pc);
        (void)uc_reg_read(uc, UC_ARM64_REG_X0, &result.x0);
        (void)uc_reg_read(uc, UC_ARM64_REG_X18, &x18);
        (void)uc_reg_read(uc, UC_ARM64_REG_X19, &x19);
        result.virtual_registers_correct = x18 == (uintptr_t)data && x19 == (uintptr_t)(data + 1);
        result.completed = error == UC_ERR_OK && result.pc == 0x100c;
        result.host_alias_correct = data[0] == UINT64_C(0x1020304050607080) &&
            data[1] == UINT64_C(0x1020304050607081) && result.x0 == data[1];
        uc_close(uc);
    }
    return result;
}

__attribute__((visibility("default"))) int native_virtual_memory_probe(char *output, size_t capacity) {
    if (!output || capacity < 2048) return -1;
    long granule = sysconf(_SC_PAGESIZE);
    if (granule < 4096) return -1;
    size_t page = (size_t)granule;
    void *code = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t *data = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED || data == MAP_FAILED) {
        if (code != MAP_FAILED) munmap(code, page);
        if (data != MAP_FAILED) munmap(data, page);
        snprintf(output, capacity, "{\"state\":\"failed\",\"error\":\"host-mapping\"}");
        return 1;
    }
    const uint32_t words[] = {0xf9400240u, 0x91000400u, 0xf9000260u};
    /* LDR X0,[X18]; ADD X0,X0,#1; STR X0,[X19]. */
    memcpy(code, words, sizeof words);
    unsigned pointer_bits = 0;
    for (uintptr_t remaining = (uintptr_t)data; remaining; remaining >>= 1) ++pointer_bits;
    int above_44 = pointer_bits > 44;
    Result old = exercise(code, data, page, 0);
    Result identity = exercise(code, data, page, 1);
    int old_rejected = above_44 && old.error == UC_ERR_EXCEPTION && !old.completed;
    int old_correct = old.completed && old.host_alias_correct && old.virtual_registers_correct;
    int passed = old_correct && identity.completed && identity.host_alias_correct && identity.virtual_registers_correct;
    unsigned major = 0, minor = 0;
    unsigned version = uc_version(&major, &minor);
    snprintf(output, capacity,
        "{\"schema\":1,\"state\":\"%s\",\"host_address\":\"%llx\",\"host_page_bytes\":%zu,\"actual_pointer_bits\":%u,\"above_44_bits_observed\":%s,\"tested_actual_host_pointer\":true,\"unicorn_version\":%u,\"unicorn_major\":%u,\"unicorn_minor\":%u,"
        "\"default_a72\":{\"model\":%d,\"error\":%d,\"error_name\":\"%s\",\"pc\":\"%llx\",\"rejected_high_address\":%s,\"completed\":%s,\"host_alias_correct\":%s,\"x18_x19_correct\":%s},"
        "\"virtual_tlb\":{\"model\":%d,\"error\":%d,\"pc\":\"%llx\",\"completed\":%s,\"host_alias_correct\":%s,\"x18_x19_correct\":%s},"
        "\"game_execution\":false,\"production_kernel_execution\":false}",
        passed ? "passed" : "failed", (unsigned long long)(uintptr_t)data, page, pointer_bits, above_44 ? "true" : "false", version, major, minor,
        old.model, old.error, uc_strerror((uc_err)old.error), (unsigned long long)old.pc, old_rejected ? "true" : "false",
        old.completed ? "true" : "false", old.host_alias_correct ? "true" : "false", old.virtual_registers_correct ? "true" : "false",
        identity.model, identity.error, (unsigned long long)identity.pc,
        identity.completed ? "true" : "false", identity.host_alias_correct ? "true" : "false",
        identity.virtual_registers_correct ? "true" : "false");
    munmap(code, page); munmap(data, page);
    return passed ? 0 : 1;
}

#ifdef NATIVE_VIRTUAL_MEMORY_MAIN
int main(void) {
    char output[4096];
    int status = native_virtual_memory_probe(output, sizeof output);
    puts(output);
    return status != 0;
}
#endif
