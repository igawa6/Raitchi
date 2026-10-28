/* Controlled embedding of the existing production loader/kernel lifecycle.
 * No replacement KThread or guest initializer is supplied by this layer. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "guest_session.h"
#include "kernel.h"
#include "runtime/aot_dispatch.h"
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "platform/windows_threads.h"
#endif

enum { EMBED_EMPTY, EMBED_PREPARING, EMBED_READY, EMBED_FAILED, EMBED_STOPPED };
static pthread_mutex_t embed_lock = PTHREAD_MUTEX_INITIALIZER;
static int embed_state;

static enum SharedGuestStatus explain(char *why, size_t capacity,
                                    enum SharedGuestStatus status, const char *message) {
    if (why && capacity) snprintf(why, capacity, "%s", message);
    return status;
}

enum SharedGuestStatus shared_guest_prepare(const char *exefs, char *why, size_t capacity) {
    if (why && capacity) *why = 0;
    if (!exefs || !*exefs) return explain(why, capacity, SHARED_GUEST_ARGUMENT, "missing executable directory");
    pthread_mutex_lock(&embed_lock);
    if (embed_state != EMBED_EMPTY) {
        pthread_mutex_unlock(&embed_lock);
        return explain(why, capacity, SHARED_GUEST_ALREADY_USED, "headless setup requires a fresh process");
    }
    const TitleManifest *manifest = shared_manifest_current();
    const NativeManifest *native = shared_native_manifest_current();
    const TitleAdapter *adapter = title_adapter_selected();
    if (!shared_session_ready() || !adapter || !session_layout_current() ||
        !aot_dispatch_is_bound() || (native ? native->title_id : manifest->title_id) != (uint32_t)adapter->id ||
        !adapter->module_count || adapter->module_count > MODULE_ABI_MAX_MODULES ||
        (native ? native->module_count : manifest->profile.module_count) != adapter->module_count) {
        pthread_mutex_unlock(&embed_lock);
        return explain(why, capacity, SHARED_GUEST_NO_SESSION, "verified shared session is required first");
    }
    if (g_nmodules || kernel_current_thread()) {
        pthread_mutex_unlock(&embed_lock);
        return explain(why, capacity, SHARED_GUEST_ALREADY_USED, "loader or guest thread is already active");
    }
    embed_state = EMBED_PREPARING;
    char directory[PATH_MAX];
    struct stat info;
    bool valid = realpath(exefs, directory) && !stat(directory, &info) && S_ISDIR(info.st_mode);
    for (uint32_t i = 0; valid && i < adapter->module_count; ++i) {
        char path[PATH_MAX];
        uint8_t digest[32];
        int length = snprintf(path, sizeof path, "%s/%s", directory, adapter->modules[i].name);
        valid = length > 0 && (size_t)length < sizeof path && shared_sha256_file(path, digest) &&
            !memcmp(digest, native ? native->modules[i].nso_sha256 : manifest->modules[i].nso_sha256, sizeof digest);
    }
    if (!valid) {
        embed_state = EMBED_FAILED;
        pthread_mutex_unlock(&embed_lock);
        return explain(why, capacity, SHARED_GUEST_INPUT, "original executable identity rejected");
    }
    /* The loader revalidates complete NSOs and retains its fail-closed fatal
     * policy for malformed/racing inputs. No original init function is called. */
    loader_load(directory);
    kernel_init();
    embed_state = EMBED_READY;
    pthread_mutex_unlock(&embed_lock);
    return SHARED_GUEST_OK;
}

static bool snapshot_valid(const SharedGuestThreadSnapshot *snapshot) {
    return snapshot && snapshot->struct_size == sizeof *snapshot &&
        snapshot->version == SHARED_GUEST_API_VERSION;
}
static KThread *thread_for_handle(uint32_t handle) {
    /* Pseudo handles dereference t_cur in handle_get, so never forward them. */
    if (handle < 0x101u || handle >= 0x1100u) return NULL;
    KObject *object = handle_get(handle);
    return object && object->type == KOBJ_THREAD ? (KThread *)object : NULL;
}
static void snapshot_thread(KThread *thread, SharedGuestThreadSnapshot *snapshot) {
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->struct_size = sizeof *snapshot;
    snapshot->version = SHARED_GUEST_API_VERSION;
    snapshot->handle = thread->handle;
    snapshot->core = thread->core;
    snapshot->priority = -1;
    snapshot->thread_id = thread->id;
    snapshot->tls_base = thread->tlr;
    snapshot->cpu_address = (uint64_t)(uintptr_t)&thread->cpu;
    snapshot->entry = thread->entry;
    if (thread->started) snapshot->flags |= SHARED_GUEST_THREAD_STARTED;
    if (thread->finished) snapshot->flags |= SHARED_GUEST_THREAD_FINISHED;
    if (thread->obj.signaled) snapshot->flags |= SHARED_GUEST_THREAD_SIGNALED;
    if (thread->joined) snapshot->flags |= SHARED_GUEST_THREAD_JOINED;
    bool current = kernel_current_thread() == thread;
    if (current) snapshot->flags |= SHARED_GUEST_THREAD_CURRENT;
    if (!thread->started || thread->finished || current) {
        snapshot->flags |= SHARED_GUEST_THREAD_CPU_STATE_VALID;
        snapshot->priority = thread->prio;
        snapshot->guest_sp = thread->cpu.sp;
        snapshot->guest_tpidr = thread->cpu.tpidr;
    }
}

enum SharedGuestStatus shared_guest_query(uint32_t handle, SharedGuestThreadSnapshot *snapshot) {
    if (!snapshot_valid(snapshot)) return SHARED_GUEST_ARGUMENT;
    pthread_mutex_lock(&embed_lock);
    if (embed_state != EMBED_READY) {
        pthread_mutex_unlock(&embed_lock);
        return SHARED_GUEST_NOT_READY;
    }
    pthread_mutex_lock(&g_klock);
    KThread *thread = thread_for_handle(handle);
    if (thread) snapshot_thread(thread, snapshot);
    pthread_mutex_unlock(&g_klock);
    pthread_mutex_unlock(&embed_lock);
    return thread ? SHARED_GUEST_OK : SHARED_GUEST_INVALID_HANDLE;
}

enum SharedGuestStatus shared_guest_current(const Cpu *expected, SharedGuestThreadSnapshot *snapshot) {
    if (!expected || !snapshot_valid(snapshot)) return SHARED_GUEST_ARGUMENT;
    pthread_mutex_lock(&embed_lock);
    if (embed_state != EMBED_READY) {
        pthread_mutex_unlock(&embed_lock);
        return SHARED_GUEST_NOT_READY;
    }
    KThread *thread = kernel_current_thread();
    bool matches = thread && kernel_current_cpu() == expected && expected->thread == thread;
    if (matches) {
        pthread_mutex_lock(&g_klock);
        snapshot_thread(thread, snapshot);
        pthread_mutex_unlock(&g_klock);
    }
    pthread_mutex_unlock(&embed_lock);
    return matches ? SHARED_GUEST_OK : SHARED_GUEST_CURRENT_MISMATCH;
}

enum SharedGuestStatus shared_guest_join_finished(uint32_t handle, SharedGuestThreadSnapshot *snapshot) {
    if (!snapshot_valid(snapshot)) return SHARED_GUEST_ARGUMENT;
    pthread_mutex_lock(&embed_lock);
    if (embed_state != EMBED_READY) {
        pthread_mutex_unlock(&embed_lock);
        return SHARED_GUEST_NOT_READY;
    }
    pthread_mutex_lock(&g_klock);
    KThread *thread = thread_for_handle(handle);
    if (!thread) {
        pthread_mutex_unlock(&g_klock);
        pthread_mutex_unlock(&embed_lock);
        return SHARED_GUEST_INVALID_HANDLE;
    }
    if (!thread->started || !thread->finished || kernel_current_thread() == thread) {
        snapshot_thread(thread, snapshot);
        pthread_mutex_unlock(&g_klock);
        pthread_mutex_unlock(&embed_lock);
        return SHARED_GUEST_IN_PROGRESS;
    }
    bool joined = thread->joined;
    pthread_mutex_unlock(&g_klock);
    int error = 0;
    if (!joined) {
#ifdef _WIN32
        error = !guest_thread_join(thread->host);
#else
        error = pthread_join(thread->host, NULL);
#endif
    }
    pthread_mutex_lock(&g_klock);
    if (!error) thread->joined = true;
    snapshot_thread(thread, snapshot);
    pthread_mutex_unlock(&g_klock);
    pthread_mutex_unlock(&embed_lock);
    return error ? SHARED_GUEST_HOST_ERROR : SHARED_GUEST_OK;
}

enum SharedGuestStatus shared_guest_shutdown(void) {
    pthread_mutex_lock(&embed_lock);
    if (embed_state != EMBED_READY) {
        pthread_mutex_unlock(&embed_lock);
        return SHARED_GUEST_NOT_READY;
    }
    if (kernel_current_thread() || !kernel_threads_all_finished()) {
        pthread_mutex_unlock(&embed_lock);
        return SHARED_GUEST_IN_PROGRESS;
    }
    kernel_stop_threads(NULL);
    embed_state = EMBED_STOPPED;
    pthread_mutex_unlock(&embed_lock);
    return SHARED_GUEST_OK;
}

const char *shared_guest_status_string(enum SharedGuestStatus status) {
    switch (status) {
    case SHARED_GUEST_OK: return "ok";
    case SHARED_GUEST_ARGUMENT: return "invalid headless API argument";
    case SHARED_GUEST_NO_SESSION: return "verified shared session required";
    case SHARED_GUEST_ALREADY_USED: return "fresh process required";
    case SHARED_GUEST_NOT_READY: return "headless session is not ready";
    case SHARED_GUEST_INPUT: return "original executable identity rejected";
    case SHARED_GUEST_INVALID_HANDLE: return "invalid thread handle";
    case SHARED_GUEST_IN_PROGRESS: return "guest thread is still active";
    case SHARED_GUEST_HOST_ERROR: return "host thread operation failed";
    case SHARED_GUEST_CURRENT_MISMATCH: return "current production CPU does not match";
    default: return "unknown headless API status";
    }
}
