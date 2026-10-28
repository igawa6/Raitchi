/* Shared runtime overlay derived from LAS Recomp Android at
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Android uint32/read-only snapshot adaptation also derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * MIT notices: native/runtime/LICENSE-LASRecomp and LICENSE-LetsGo. */
#pragma once
#include <stdbool.h>
#include <pthread.h>

typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
// A queue for its threads: the game's own work first, the shader head start's (bg) when there's none
typedef struct {
    Job *head, *tail, *bhead, *btail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_once_t once;
    int bg_running, bg_max;  // (head start jobs at most bg_max at a time: the rest of the threads stay the game's)
} JobQueue;
// Workers run for the lifetime of the fresh game process.
void shader_job_queue_run(JobQueue *queue);
// False leaves the queue untouched when a job node cannot be allocated.
bool shader_job_queue_push(JobQueue *queue, void (*fn)(void *), void *arg, bool background);
