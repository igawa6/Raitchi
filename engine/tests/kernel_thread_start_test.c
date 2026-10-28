/* Runs the verbatim production thread_start extracted by the checker. Only
 * pthread attribute/create operations are controlled; no guest thread runs.
 * Uses the actual private KThread declaration, not a duplicate layout. */
#include "kernel.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint assertions;
static unsigned init_calls, size_calls, create_calls, destroy_calls;
static int failure;

#define CHECK(condition) do { ++assertions; if (!(condition)) { \
    fprintf(stderr, "thread_start line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

static void *thread_main(void *argument) { return argument; }
static int controlled_attr_init(pthread_attr_t *attribute) {
    ++init_calls;
    if (failure == 1) return ENOMEM;
    memset(attribute, 0, sizeof *attribute);
    return 0;
}
static int controlled_stacksize(pthread_attr_t *attribute, size_t bytes) {
    CHECK(attribute != NULL && bytes == (256ull << 20));
    ++size_calls;
    return failure == 2 ? EINVAL : 0;
}
static int controlled_create(pthread_t *host, const pthread_attr_t *attribute,
                             void *(*entry)(void *), void *argument) {
    CHECK(attribute && entry == thread_main && argument);
    CHECK(pthread_mutex_trylock(&g_klock) == EBUSY);
    ++create_calls;
    if (failure == 3) return EAGAIN;
    *host = pthread_self(); /* A token only; no actual guest callback is invoked. */
    return 0;
}
static int controlled_destroy(pthread_attr_t *attribute) {
    CHECK(attribute != NULL);
    ++destroy_calls;
    return 0;
}

#define pthread_attr_init controlled_attr_init
#define pthread_attr_setstacksize controlled_stacksize
#define pthread_create controlled_create
#define pthread_attr_destroy controlled_destroy
#include "production_thread_start.inc"
#undef pthread_attr_init
#undef pthread_attr_setstacksize
#undef pthread_create
#undef pthread_attr_destroy

static void check_unlocked(void) {
    CHECK(pthread_mutex_trylock(&g_klock) == 0);
    CHECK(pthread_mutex_unlock(&g_klock) == 0);
}
static void reset(int fail) {
    failure = fail;
    init_calls = size_calls = create_calls = destroy_calls = 0;
}
typedef struct Call {
    KThread *thread;
    pthread_barrier_t *barrier;
    uint32_t result;
} Call;
static void *start_concurrently(void *argument) {
    Call *call = argument;
    pthread_barrier_wait(call->barrier);
    call->result = thread_start(call->thread);
    return NULL;
}

int main(void) {
    for (int fail = 1; fail <= 3; ++fail) {
        reset(fail);
        KThread thread = {0};
        CHECK(thread_start(&thread) == R_OUT_OF_RESOURCE);
        CHECK(!thread.started && !thread.finished && !thread.joined);
        CHECK(init_calls == 1);
        CHECK(size_calls == (fail >= 2 ? 1u : 0u));
        CHECK(create_calls == (fail == 3 ? 1u : 0u));
        CHECK(destroy_calls == (fail >= 2 ? 1u : 0u));
        check_unlocked();
        /* Failed creation must remain retryable, then succeed exactly once. */
        unsigned previous_creates = create_calls, previous_destroys = destroy_calls;
        failure = 0;
        CHECK(thread_start(&thread) == R_OK && thread.started);
        CHECK(create_calls == previous_creates + 1 && destroy_calls == previous_destroys + 1);
        CHECK(thread_start(&thread) == R_INVALID_STATE);
        CHECK(create_calls == previous_creates + 1 && destroy_calls == previous_destroys + 1);
        check_unlocked();
    }
    for (unsigned state = 1; state < 8; ++state) {
        reset(0);
        KThread thread = {.started = !!(state & 1), .finished = !!(state & 2), .joined = !!(state & 4)};
        CHECK(thread_start(&thread) == R_INVALID_STATE);
        CHECK(thread.started == !!(state & 1) && thread.finished == !!(state & 2) && thread.joined == !!(state & 4));
        CHECK(!init_calls && !size_calls && !create_calls && !destroy_calls);
        check_unlocked();
    }
    reset(0);
    KThread thread = {0};
    CHECK(thread_start(&thread) == R_OK);
    CHECK(thread.started && !thread.finished && !thread.joined);
    CHECK(init_calls == 1 && size_calls == 1 && create_calls == 1 && destroy_calls == 1);
    check_unlocked();

    /* Actual concurrent host callers must not both create the same KThread. */
    reset(0);
    KThread concurrent = {0};
    pthread_barrier_t barrier;
    CHECK(!pthread_barrier_init(&barrier, NULL, 2));
    Call calls[2] = {{&concurrent, &barrier, 0}, {&concurrent, &barrier, 0}};
    pthread_t callers[2];
    for (unsigned i = 0; i < 2; ++i) CHECK(!pthread_create(&callers[i], NULL, start_concurrently, &calls[i]));
    for (unsigned i = 0; i < 2; ++i) CHECK(!pthread_join(callers[i], NULL));
    CHECK((calls[0].result == R_OK && calls[1].result == R_INVALID_STATE) ||
          (calls[1].result == R_OK && calls[0].result == R_INVALID_STATE));
    CHECK(concurrent.started && init_calls == 1 && size_calls == 1 && create_calls == 1 && destroy_calls == 1);
    CHECK(!pthread_barrier_destroy(&barrier));
    check_unlocked();
    printf("production thread_start assertions=%u; controlled pthread creation only\n", atomic_load(&assertions));
    return 0;
}
