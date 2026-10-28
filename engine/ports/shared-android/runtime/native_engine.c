/* Original NSO execution: native safe prefixes, Unicorn for other instructions.
 * No generated guest function is called by this engine. Host callbacks are
 * restricted by the coordinator's host-only lookup contract. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native_engine.h"
#include "kernel.h"
#include "native-game-backend/block_plan.h"
#include "native-game-backend/control_step.h"
#include "native-game-backend/atomic_step.h"
#include "native-game-backend/plan_cache.h"
#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct EngineContext {
    Cpu *cpu;
    uc_engine *uc;
    uc_hook memory_hook;
    NgbDecoder decoder;
    NgbAtomicMonitor monitor;
    NgbPlanCache plan_cache;
    unsigned depth;
    uint64_t fault_address;
    int fault_type;
    uint64_t instructions, next_log;
    uint64_t iterations, interpreted_steps, host_calls, map_count, mapped_bytes, invocations;
    uint64_t last_symbol_trace_ns;
    unsigned id;
    struct EngineContext *next;
} EngineContext;
static pthread_mutex_t engine_lock = PTHREAD_MUTEX_INITIALIZER;
static EngineContext *contexts;
static NativeEngineHooks engine_hooks;
static atomic_bool configured;
static uint64_t log_interval = UINT64_C(1000000);
static atomic_uint context_count;
static uint64_t trace_symbol_begin, trace_symbol_end;
static size_t original_text_available(uint64_t pc);
#define ENGINE_TCG_BYTES (UINT32_C(8) << 20)
#define ENGINE_MAX_MIRROR_BYTES (UINT64_C(512) << 20)
#define COUNTERS(X) X(invocations) X(native_blocks) X(native_instructions) \
    X(interpreted_instructions) X(native_declines) X(software_instructions) \
    X(main_native_instructions) X(sdk_native_instructions) X(atomic_instructions) \
    X(hle_calls) X(svc_calls) X(instruction_hooks) \
    X(mapped_pages) X(failures) X(aot_calls) X(plan_cache_hits) X(plan_cache_misses) X(plan_cache_invalidations)
#define DEFINE_COUNTER(name) static _Atomic uint64_t counter_##name;
COUNTERS(DEFINE_COUNTER)
#undef DEFINE_COUNTER

static bool early_trace(uint64_t count) {
    return count <= 16 || (count <= 1024 && !(count & (count - 1)));
}
static void memory_snapshot(const char *stage, const EngineContext *context) {
    uint64_t rss = 0, virtual_size = 0, anonymous = 0;
    FILE *status = fopen("/proc/self/status", "r");
    if (status) {
        char line[256];
        unsigned long long value;
        while (fgets(line, sizeof line, status)) {
            if (sscanf(line, "VmRSS: %llu kB", &value) == 1) rss = value;
            else if (sscanf(line, "VmSize: %llu kB", &value) == 1) virtual_size = value;
            else if (sscanf(line, "RssAnon: %llu kB", &value) == 1) anonymous = value;
        }
        fclose(status);
    }
    LOG("[native-engine-memory] stage=%s context=%u contexts=%u cpu=%p uc=%p depth=%u iterations=%llu instructions=%llu maps=%llu mirror_bytes=%llu rss_kb=%llu anon_kb=%llu virtual_kb=%llu\n",
        stage, context ? context->id : 0, atomic_load_explicit(&context_count, memory_order_relaxed), context ? (void *)context->cpu : NULL,
        context ? (void *)context->uc : NULL, context ? context->depth : 0,
        (unsigned long long)(context ? context->iterations : 0),
        (unsigned long long)(context ? context->instructions : 0),
        (unsigned long long)(context ? context->map_count : 0),
        (unsigned long long)(context ? context->mapped_bytes : 0),
        (unsigned long long)rss, (unsigned long long)anonymous, (unsigned long long)virtual_size);
}

__attribute__((noreturn)) static void failed(EngineContext *context, uint64_t pc, uint32_t word,
                   const char *operation, int status) {
    atomic_fetch_add_explicit(&counter_failures, 1, memory_order_relaxed);
    FATAL("[native-engine] %s pc=%llx word=%08x status=%d (%s) fault=%llx type=%d depth=%u atomic_monitor=%u\n",
          operation, (unsigned long long)pc, word, status, uc_strerror((uc_err)status),
          (unsigned long long)(context ? context->fault_address : 0), context ? context->fault_type : 0,
          context ? context->depth : 0, context ? context->monitor.valid : 0);
}

int native_engine_configure(const NativeEngineHooks *hooks) {
    if (!hooks || !hooks->host_lookup || !hooks->run_native_block || !hooks->safe_point ||
        !hooks->instruction_hook || !hooks->native_limit) return -1;
    pthread_mutex_lock(&engine_lock);
    if (atomic_load_explicit(&configured, memory_order_relaxed)) {
        pthread_mutex_unlock(&engine_lock);
        return -1;
    }
    engine_hooks = *hooks;
    const char *interval = getenv("LARECOMP_NATIVE_LOG_INTERVAL");
    if (interval && *interval) {
        char *end;
        uint64_t value = strtoull(interval, &end, 10);
        if (!*end && value >= 1000) log_interval = value;
    }
    const char *trace_symbol = getenv("LARECOMP_NATIVE_TRACE_SYMBOL");
    if (trace_symbol && *trace_symbol) {
        size_t bytes = 512;
        const char *trace_bytes = getenv("LARECOMP_NATIVE_TRACE_BYTES");
        if (trace_bytes && *trace_bytes) {
            char *end;
            unsigned long long value = strtoull(trace_bytes, &end, 10);
            if (!*end && value >= 4 && value <= 4096) bytes = (size_t)value & ~(size_t)3;
        }
        uint64_t address = loader_find_symbol(trace_symbol);
        size_t available = address ? original_text_available(address) : 0;
        if (available >= 4) {
            trace_symbol_begin = address;
            trace_symbol_end = address + (bytes < available ? bytes : available);
            LOG("[native-symbol-trace] configured symbol=%.256s begin=%llx bytes=%llu rate_per_context_hz=1\n",
                trace_symbol, (unsigned long long)address, (unsigned long long)(trace_symbol_end - address));
        } else {
            LOG("[native-symbol-trace] disabled: symbol absent from current original text: %.256s\n", trace_symbol);
        }
    }
    (void)rt_ticks(); /* Initialize production timer configuration before threads. */
    atomic_store_explicit(&configured, true, memory_order_release);
    pthread_mutex_unlock(&engine_lock);
    return 0;
}

static void trace_symbol_state(EngineContext *context) {
    Cpu *cpu = context->cpu;
    if (!trace_symbol_begin || cpu->pc < trace_symbol_begin || cpu->pc >= trace_symbol_end) return;
    uint64_t now = plat_now_ns();
    if (context->last_symbol_trace_ns && now - context->last_symbol_trace_ns < UINT64_C(1000000000)) return;
    context->last_symbol_trace_ns = now;
    uint32_t word;
    memcpy(&word, (const void *)(uintptr_t)cpu->pc, sizeof word);
    /* Cpu is materialized at this executing thread's C boundary. pc/word name
     * the NEXT original instruction, with state after the preceding operation. */
    LOG("[native-symbol-trace] context=%u instructions=%llu pc=%llx offset=%llx word=%08x x0=%llx x8=%llx x9=%llx x10=%llx x11=%llx x14=%llx x15=%llx x16=%llx x17=%llx x5=%llx x6=%llx x18=%llx\n",
        context->id, (unsigned long long)context->instructions, (unsigned long long)cpu->pc,
        (unsigned long long)(cpu->pc - trace_symbol_begin), word,
        (unsigned long long)cpu->x[0], (unsigned long long)cpu->x[8],
        (unsigned long long)cpu->x[9], (unsigned long long)cpu->x[10],
        (unsigned long long)cpu->x[11], (unsigned long long)cpu->x[14],
        (unsigned long long)cpu->x[15], (unsigned long long)cpu->x[16],
        (unsigned long long)cpu->x[17], (unsigned long long)cpu->x[5],
        (unsigned long long)cpu->x[6], (unsigned long long)cpu->x[18]);
}

static void progress(EngineContext *context, uint64_t count) {
    context->instructions += count;
    if (trace_symbol_begin) trace_symbol_state(context);
    if (context->instructions < context->next_log && !early_trace(context->instructions)) return;
    NativeEngineStats statistics;
    native_engine_stats(&statistics);
    LOG("[native-engine-progress] {\"pc\":\"%llx\",\"thread\":%u,\"native\":%llu,\"main_native\":%llu,\"sdk_native\":%llu,\"software\":%llu,\"atomic\":%llu,\"unicorn\":%llu,\"hle\":%llu,\"svc\":%llu,\"declines\":%llu,\"engine_aot_calls\":%llu,\"plan_hits\":%llu,\"plan_misses\":%llu,\"plan_invalidations\":%llu}\n",
        (unsigned long long)context->cpu->pc, kernel_current_thread_handle(),
        (unsigned long long)statistics.native_instructions,
        (unsigned long long)statistics.main_native_instructions,
        (unsigned long long)statistics.sdk_native_instructions,
        (unsigned long long)statistics.software_instructions,
        (unsigned long long)statistics.atomic_instructions,
        (unsigned long long)statistics.interpreted_instructions,
        (unsigned long long)statistics.hle_calls, (unsigned long long)statistics.svc_calls,
        (unsigned long long)statistics.native_declines, (unsigned long long)statistics.aot_calls,
        (unsigned long long)statistics.plan_cache_hits, (unsigned long long)statistics.plan_cache_misses,
        (unsigned long long)statistics.plan_cache_invalidations);
    context->next_log = context->instructions + log_interval;
    memory_snapshot("progress", context);
}

void native_engine_stats(NativeEngineStats *out) {
    if (!out) return;
#define READ_COUNTER(name) out->name = atomic_load_explicit(&counter_##name, memory_order_relaxed);
    COUNTERS(READ_COUNTER)
#undef READ_COUNTER
}

/* Inspect the actual host mapping before exposing its pointer to Unicorn.
 * Guest permission bookkeeping alone does not establish a valid host pointer. */
static bool host_mapping(uint64_t address, uint64_t *lo, uint64_t *hi, unsigned *permissions) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[1024], protection[5];
    unsigned long long begin, end;
    bool found = false;
    while (fgets(line, sizeof line, maps)) {
        if (sscanf(line, "%llx-%llx %4s", &begin, &end, protection) != 3 ||
            address < begin || address >= end) continue;
        if (protection[0] != 'r') break;
        *lo = begin; *hi = end;
        /* Original text is host RW, not executable. Unicorn instruction fetch
         * is allowed from readable backing; direct native execution uses RX islands. */
        *permissions = UC_PROT_READ | UC_PROT_EXEC | (protection[1] == 'w' ? UC_PROT_WRITE : 0);
        found = true;
        break;
    }
    fclose(maps);
    return found;
}

static bool memory_fault(uc_engine *uc, uc_mem_type type, uint64_t address,
                         int size, int64_t value, void *opaque) {
    (void)value;
    EngineContext *context = opaque;
    context->fault_address = address; context->fault_type = type;
    if (size <= 0 || address > UINT64_MAX - (uint64_t)size) return false;
    uint64_t lo, hi; unsigned permissions;
    if (!host_mapping(address, &lo, &hi, &permissions)) return false;
    const SessionLayout *layout = session_layout_current();
    bool app_heap = layout && address >= layout->regions[SESSION_APP_HEAP].lo &&
        address < layout->regions[SESSION_APP_HEAP].hi;
    /* The renderer deliberately write-protects heap pages. Give Unicorn a
     * writable view so its actual host-pointer store reaches the production
     * SIGSEGV dirty-tracking handler, exactly like native/C-AOT stores. */
    if (app_heap) permissions |= UC_PROT_WRITE;
    bool write = type == UC_MEM_WRITE_UNMAPPED || type == UC_MEM_WRITE_PROT;
    if (write && !(permissions & UC_PROT_WRITE)) return false;
    long host_page = sysconf(_SC_PAGESIZE);
    if (host_page < 4096 || ((unsigned long)host_page & ((unsigned long)host_page - 1))) return false;
    uint64_t page = address & ~((uint64_t)host_page - 1);
    if (page < lo || page + (uint64_t)host_page > hi) return false;
    if (type == UC_MEM_WRITE_PROT || type == UC_MEM_READ_PROT || type == UC_MEM_FETCH_PROT)
        return uc_mem_protect(uc, page, (size_t)host_page, permissions) == UC_ERR_OK;
    /* Fixed 64KiB buckets keep lazy maps disjoint while reducing /proc overhead.
     * Clamp to the host VMA, whose boundaries are host-page aligned. */
    uint64_t begin = address & ~UINT64_C(65535), end = begin + UINT64_C(65536);
    if (begin < lo) begin = lo;
    if (end > hi) end = hi;
    if (end <= begin || end - begin > UINT64_C(65536) ||
        context->mapped_bytes > ENGINE_MAX_MIRROR_BYTES - (end - begin)) return false;
    if (early_trace(context->map_count + 1)) {
        LOG("[native-engine-map] context=%u type=%d address=%llx size=%d backing=%llx..%llx bucket=%llx..%llx perms=%u\n",
            context->id, type, (unsigned long long)address, size,
            (unsigned long long)lo, (unsigned long long)hi,
            (unsigned long long)begin, (unsigned long long)end, permissions);
        memory_snapshot("before-map", context);
    }
    uc_err error = uc_mem_map_ptr(uc, begin, (size_t)(end - begin), permissions, (void *)(uintptr_t)begin);
    if (error != UC_ERR_OK) {
        /* A later host VMA split can leave part of this bucket mapped already. */
        error = uc_mem_map_ptr(uc, page, (size_t)host_page, permissions, (void *)(uintptr_t)page);
        if (error == UC_ERR_OK) end = page + (uint64_t)host_page, begin = page;
    }
    if (error != UC_ERR_OK) return false;
    ++context->map_count;
    context->mapped_bytes += end - begin;
    if (early_trace(context->map_count)) memory_snapshot("after-map", context);
    atomic_fetch_add_explicit(&counter_mapped_pages, (end - begin) / (uint64_t)host_page, memory_order_relaxed);
    return true;
}

static EngineContext *context_for(Cpu *cpu) {
    pthread_mutex_lock(&engine_lock);
    for (EngineContext *context = contexts; context; context = context->next) {
        if (context->cpu == cpu) {
            pthread_mutex_unlock(&engine_lock);
            return context;
        }
    }
    EngineContext *context = calloc(1, sizeof *context);
    if (context) {
        context->cpu = cpu;
        context->id = atomic_fetch_add_explicit(&context_count, 1, memory_order_relaxed) + 1;
    }
    memory_snapshot("before-uc-open", context);
    uc_err error = context ? uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &context->uc) : UC_ERR_NOMEM;
    if (error == UC_ERR_OK) {
        /* This setter must precede CPACR/register/hook/memory APIs: those
         * lazily initialize Unicorn, whose 64-bit default is 1GiB per Cpu. */
        error = uc_ctl_set_tcg_buffer_size(context->uc, ENGINE_TCG_BYTES);
        memory_snapshot("after-uc-open-bounded-tcg", context);
    }
    if (error == UC_ERR_OK && ngb_decoder_init(&context->decoder) != NGB_OK) error = UC_ERR_RESOURCE;
    if (error == UC_ERR_OK) {
        context->cpu = cpu;
        context->next_log = log_interval;
        uint64_t cpacr = UINT64_C(3) << 20; /* Enable the SIMD/FP register file. */
        error = uc_reg_write(context->uc, UC_ARM64_REG_CPACR_EL1, &cpacr);
        memory_snapshot("after-uc-register-init", context);
        if (error == UC_ERR_OK) {
            uint32_t actual_buffer = 0;
            error = uc_ctl_get_tcg_buffer_size(context->uc, &actual_buffer);
            LOG("[native-engine-tcg] context=%u requested=%u actual=%u\n", context->id, ENGINE_TCG_BYTES, actual_buffer);
            if (error == UC_ERR_OK && actual_buffer > ENGINE_TCG_BYTES) error = UC_ERR_RESOURCE;
        }
    }
    if (error == UC_ERR_OK)
        error = uc_hook_add(context->uc, &context->memory_hook, UC_HOOK_MEM_INVALID,
                            (void *)memory_fault, context, 1, 0);
    if (error != UC_ERR_OK) {
        pthread_mutex_unlock(&engine_lock);
        failed(context, cpu->pc, 0, "create interpreter", error);
    }
    context->next = contexts; contexts = context;
    memory_snapshot("context-ready", context);
    pthread_mutex_unlock(&engine_lock);
    return context;
}

static int gpr_id(unsigned index) {
    return index < 29 ? UC_ARM64_REG_X0 + (int)index : index == 29 ? UC_ARM64_REG_X29 : UC_ARM64_REG_X30;
}
static void registers(EngineContext *context, bool write) {
    Cpu *cpu = context->cpu;
    uc_err error = UC_ERR_OK;
#define TRANSFER(id, pointer) do { \
    error = write ? uc_reg_write(context->uc, (id), (pointer)) : uc_reg_read(context->uc, (id), (pointer)); \
    if (error != UC_ERR_OK) failed(context, cpu->pc, 0, "register transfer", error); \
} while (0)
    for (unsigned i = 0; i < 31; ++i) TRANSFER(gpr_id(i), &cpu->x[i]);
    TRANSFER(UC_ARM64_REG_SP, &cpu->sp);
    TRANSFER(UC_ARM64_REG_PC, &cpu->pc);
    TRANSFER(UC_ARM64_REG_NZCV, &cpu->nzcv);
    TRANSFER(UC_ARM64_REG_TPIDR_EL0, &cpu->tpidr);
    TRANSFER(UC_ARM64_REG_TPIDRRO_EL0, &cpu->tpidrro);
    TRANSFER(UC_ARM64_REG_FPCR, &cpu->fpcr);
    TRANSFER(UC_ARM64_REG_FPSR, &cpu->fpsr);
    for (unsigned i = 0; i < 32; ++i) TRANSFER(UC_ARM64_REG_Q0 + (int)i, &cpu->v[i]);
#undef TRANSFER
}

/* Do not execute a native prefix from arbitrary readable host memory. Original
 * module bounds come from the already identity-checked production loader. */
static size_t original_text_available(uint64_t pc) {
    if (pc & 3) return 0;
    for (int i = 0; i < g_nmodules; ++i) {
        uint64_t begin = g_modules[i].base + g_modules[i].text_off;
        if (pc >= begin && pc - begin < g_modules[i].text_size)
            return (size_t)((g_modules[i].text_size - (pc - begin)) & ~UINT64_C(3));
    }
    return 0;
}

static void interpret_one(EngineContext *context, const NgbInstruction *instruction) {
    Cpu *cpu = context->cpu;
    uint64_t pc = cpu->pc;
    /* Bundled Unicorn lowers atomics to ordinary loads/stores. Its failed CAS
     * can write a stale old value over concurrent kernel/native stores. */
    if (instruction->kind == NGB_EXIT_ATOMIC ||
        (instruction->flags & (NGB_EXCLUSIVE_LOAD | NGB_EXCLUSIVE_STORE | NGB_CLEAR_EXCLUSIVE)))
        failed(context, pc, instruction->word, "atomic instruction forbidden in Unicorn", UC_ERR_INSN_INVALID);
    registers(context, true);
    ++context->interpreted_steps;
    if (early_trace(context->interpreted_steps)) {
        LOG("[native-engine-step] context=%u pc=%llx word=%08x sp=%llx atomic_monitor=%u\n",
            context->id, (unsigned long long)pc, instruction->word, (unsigned long long)cpu->sp, context->monitor.valid);
        memory_snapshot("before-uc-step", context);
    }
    /* End exactly after this instruction as well as count=1. Avoid exposing
     * an unbounded address range to the translator's exit-address machinery. */
    uc_err error = uc_emu_start(context->uc, pc, pc + 4, 0, 1);
    if (error != UC_ERR_OK) failed(context, pc, instruction->word, "interpret original instruction", error);
    registers(context, false);
    if (early_trace(context->interpreted_steps)) memory_snapshot("after-uc-step", context);
    atomic_fetch_add_explicit(&counter_interpreted_instructions, 1, memory_order_relaxed);
    progress(context, 1);
}

void native_engine_run(Cpu *cpu) {
    if (!atomic_load_explicit(&configured, memory_order_acquire) || !cpu || kernel_current_cpu() != cpu ||
        !kernel_current_thread() || cpu->thread != kernel_current_thread())
        failed(NULL, cpu ? cpu->pc : 0, 0, "actual current production Cpu required", UC_ERR_ARG);
    EngineContext *context = context_for(cpu);
    ++context->depth;
    if (early_trace(++context->invocations)) memory_snapshot("invocation-enter", context);
    atomic_fetch_add_explicit(&counter_invocations, 1, memory_order_relaxed);
    while (cpu->pc) {
        if (g_rt_unwind) ngb_atomic_monitor_clear(&context->monitor);
        engine_hooks.safe_point();
        uint64_t pc = cpu->pc;
        ++context->iterations;
        if (early_trace(context->iterations)) {
            LOG("[native-engine-iteration] context=%u iteration=%llu pc=%llx lr=%llx sp=%llx x0=%llx x1=%llx x2=%llx x3=%llx\n",
                context->id, (unsigned long long)context->iterations, (unsigned long long)pc,
                (unsigned long long)cpu->x[30], (unsigned long long)cpu->sp,
                (unsigned long long)cpu->x[0], (unsigned long long)cpu->x[1],
                (unsigned long long)cpu->x[2], (unsigned long long)cpu->x[3]);
            memory_snapshot("iteration", context);
        }
        GuestFn host = engine_hooks.host_lookup(pc);
        if (host) {
            uint64_t return_pc = cpu->x[30];
            ++context->host_calls;
            if (early_trace(context->host_calls)) memory_snapshot("before-hle", context);
            ngb_atomic_monitor_clear(&context->monitor);
            atomic_fetch_add_explicit(&counter_hle_calls, 1, memory_order_relaxed);
            /* The UC engine is idle and no engine/kernel lock is held. HLE can
             * reenter guest_call with this actual Cpu, or ExitThread longjmp. */
            host(cpu);
            if (early_trace(context->host_calls)) memory_snapshot("after-hle", context);
            ngb_atomic_monitor_clear(&context->monitor);
            cpu->pc = return_pc;
            continue;
        }
        size_t available = original_text_available(pc);
        if (!available) failed(context, pc, 0, "PC outside original module text", UC_ERR_FETCH_UNMAPPED);
        size_t limit = engine_hooks.native_limit(cpu, pc, NGB_MAX_NATIVE_WORDS);
        if (limit > NGB_MAX_NATIVE_WORDS) failed(context, pc, 0, "invalid native instruction limit", UC_ERR_ARG);
        /* Registered hooks may mutate memory yet return0 to execute the
         * original instruction; limit0 identifies those sites before callback. */
        if (!limit) ngb_atomic_monitor_clear(&context->monitor);
        int hook = engine_hooks.instruction_hook(cpu, pc);
        if (!limit || hook) ngb_atomic_monitor_clear(&context->monitor);
        if (hook) {
            atomic_fetch_add_explicit(&counter_instruction_hooks, 1, memory_order_relaxed);
            if (hook == 1) cpu->pc = pc + 4;
            else if (hook != 2) failed(context, pc, 0, "invalid instruction hook result", UC_ERR_ARG);
            continue;
        }
        NgbBlockPlan plan = {.struct_size = sizeof plan, .version = NGB_PLAN_VERSION};
        NgbInstruction instruction;
        size_t bytes = limit ? limit * 4 : 4;
        if (bytes > available) bytes = available;
        uint64_t hits = context->plan_cache.hits, invalidations = context->plan_cache.invalidations;
        enum NgbStatus status = ngb_plan_cache_get(&context->plan_cache, &context->decoder,
            (const void *)(uintptr_t)pc, bytes, pc, (unsigned)limit, &plan, &instruction);
        if (status != NGB_OK) failed(context, plan.next_pc, plan.exit.word, "decode original block", UC_ERR_INSN_INVALID);
        if (context->plan_cache.hits != hits)
            atomic_fetch_add_explicit(&counter_plan_cache_hits, 1, memory_order_relaxed);
        else atomic_fetch_add_explicit(&counter_plan_cache_misses, 1, memory_order_relaxed);
        if (context->plan_cache.invalidations != invalidations)
            atomic_fetch_add_explicit(&counter_plan_cache_invalidations, 1, memory_order_relaxed);
        if (limit) {
            if (plan.native_count) {
                if ((plan.gpr_usage & (UINT32_C(1) << 18)) || !(NGB_CONTEXT_GPR_MASK & ~plan.gpr_usage))
                    failed(context, pc, plan.words[0], "native context-register admission failed", UC_ERR_ARG);
                int native = engine_hooks.run_native_block(cpu, plan.words, plan.native_count, plan.gpr_usage);
                if (native < 0)
                    failed(context, pc, plan.words[0], "native block failed after admission", UC_ERR_EXCEPTION);
                if (!native) {
                    cpu->pc = plan.next_pc;
                    atomic_fetch_add_explicit(&counter_native_blocks, 1, memory_order_relaxed);
                    atomic_fetch_add_explicit(&counter_native_instructions, plan.native_count, memory_order_relaxed);
                    for (int module = 0; module < g_nmodules; ++module) {
                        if (pc < g_modules[module].base || pc - g_modules[module].base >= g_modules[module].size) continue;
                        if (!strcmp(g_modules[module].name, "main"))
                            atomic_fetch_add_explicit(&counter_main_native_instructions, plan.native_count, memory_order_relaxed);
                        else if (!strcmp(g_modules[module].name, "sdk"))
                            atomic_fetch_add_explicit(&counter_sdk_native_instructions, plan.native_count, memory_order_relaxed);
                        break;
                    }
                    progress(context, plan.native_count);
                    continue;
                }
                atomic_fetch_add_explicit(&counter_native_declines, 1, memory_order_relaxed);
            }
        }
        uint32_t word;
        memcpy(&word, (const void *)(uintptr_t)pc, sizeof word);
        /* A NOT_ENTERED native attempt must use metadata for its FIRST word,
         * not the exit at the end of the planned prefix. Recheck PC/word in
         * case code changed while a resource-admission callback ran. */
        if ((instruction.pc != pc || instruction.word != word) &&
            ngb_classify(&context->decoder, &word, pc, &instruction) != NGB_OK)
            failed(context, pc, word, "decode interpreted instruction", UC_ERR_INSN_INVALID);
        int atomic = ngb_atomic_step(cpu, word, pc, &context->monitor);
        if (atomic < 0 || (!atomic && (instruction.kind == NGB_EXIT_ATOMIC ||
            (instruction.flags & (NGB_EXCLUSIVE_LOAD | NGB_EXCLUSIVE_STORE | NGB_CLEAR_EXCLUSIVE)))))
            failed(context, pc, word, "unsupported or misaligned host atomic", UC_ERR_INSN_INVALID);
        if (atomic) {
            atomic_fetch_add_explicit(&counter_atomic_instructions, 1, memory_order_relaxed);
            progress(context, 1);
            continue;
        }
        if ((word & UINT32_C(0xffe0001f)) == UINT32_C(0xd4000001)) {
            ngb_atomic_monitor_clear(&context->monitor);
            atomic_fetch_add_explicit(&counter_svc_calls, 1, memory_order_relaxed);
            rt_svc(cpu, (word >> 5) & 0xffffu, pc);
            ngb_atomic_monitor_clear(&context->monitor);
            cpu->pc = pc + 4;
            progress(context, 1);
            continue;
        }
        if (ngb_control_step(cpu, word, pc, rt_ticks())) {
            atomic_fetch_add_explicit(&counter_software_instructions, 1, memory_order_relaxed);
            progress(context, 1);
            continue;
        }
        interpret_one(context, &instruction);
    }
    --context->depth;
}
