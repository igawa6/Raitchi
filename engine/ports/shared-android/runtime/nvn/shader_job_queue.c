/* Shared runtime overlay derived from LAS Recomp Android at
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Android uint32/read-only snapshot adaptation also derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * MIT notices: native/runtime/LICENSE-LASRecomp and LICENSE-LetsGo. */
#include "shader_job_queue.h"
#include <stdlib.h>

void shader_job_queue_run(JobQueue *q) {
    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (!q->head && !(q->bhead && q->bg_running < q->bg_max)) pthread_cond_wait(&q->cond, &q->lock);
        bool bg = !q->head;
        Job **h = bg ? &q->bhead : &q->head, **t = bg ? &q->btail : &q->tail;
        Job *j = *h;
        *h = j->next;
        if (!*h) *t = NULL;
        if (bg) q->bg_running++;
        pthread_mutex_unlock(&q->lock);
        j->fn(j->arg);
        free(j);
        if (bg) {
            pthread_mutex_lock(&q->lock);
            q->bg_running--;
            pthread_cond_broadcast(&q->cond);
            pthread_mutex_unlock(&q->lock);
        }
    }
}

bool shader_job_queue_push(JobQueue *q, void (*fn)(void *), void *arg, bool bg) {
    Job *j = malloc(sizeof *j);
    if (!j) return false;
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&q->lock);
    Job **h = bg ? &q->bhead : &q->head, **t = bg ? &q->btail : &q->tail;
    if (*t) (*t)->next = j; else *h = j;
    *t = j;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return true;
}

