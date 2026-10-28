/* Exercise real queue allocation and translator-start failure paths. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static bool fail_allocation;
static void *job_malloc(size_t size) {
    return fail_allocation ? NULL : malloc(size);
}
#define malloc job_malloc
#include "nvn/shader_job_queue.c"
#undef malloc

static jmp_buf failure;
static unsigned fatal_count, creates, detaches;
static int create_result;
static void *translator(void *unused) { return unused; }
static int create_thread(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*entry)(void *), void *arg) {
    assert(!attr && entry == translator && !arg);
    creates++;
    *thread = (pthread_t)1;
    return create_result;
}
static int detach_thread(pthread_t thread) {
    assert(thread == (pthread_t)1);
    detaches++;
    return 0;
}
#define FATAL(...) do { fatal_count++; longjmp(failure, 1); } while (0)
#define pthread_create create_thread
#define pthread_detach detach_thread
#include "shader_job_wrappers.inc"
#undef pthread_create
#undef pthread_detach

static void noop(void *arg) { (void)arg; }

int main(void) {
    JobQueue queue = {.lock = PTHREAD_MUTEX_INITIALIZER,
                      .cond = PTHREAD_COND_INITIALIZER};
    fail_allocation = true;
    assert(!shader_job_queue_push(&queue, noop, NULL, false));
    assert(!queue.head && !queue.tail && !queue.bhead && !queue.btail);
    if (!setjmp(failure)) {
        queue_push(&queue, noop, NULL, true);
        abort();
    }
    assert(fatal_count == 1 && !queue.bhead && !queue.btail);
    fail_allocation = false;
    assert(shader_job_queue_push(&queue, noop, &queue, false));
    assert(queue.head == queue.tail && queue.head->arg == &queue);
    free(queue.head);
    queue.head = queue.tail = NULL;

    create_result = EAGAIN;
    if (!setjmp(failure)) {
        start_translator();
        abort();
    }
    assert(fatal_count == 2 && creates == 1 && detaches == 0);
    create_result = 0;
    start_translator();
    assert(creates == 2 && detaches == 1 && fatal_count == 2);
    assert(!pthread_cond_destroy(&queue.cond));
    assert(!pthread_mutex_destroy(&queue.lock));
    puts("allocation failure preserves queue; translator failure terminates; successful translator detaches");
    return 0;
}
