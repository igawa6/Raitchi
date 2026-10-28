/* Derived from LAS Recomp Android cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c)2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp. */
// The render thread (runtime/nvn/rqueue.c): command lists the game submits are translated and run by a thread of
// our own, so the game's GPU thread goes on with the next frame while this one translates. Off (every call below
// runs the backend directly on the calling thread): LARECOMP_RENDER_THREAD=0.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "nvn.h"
#include "command_lifetime.h"

bool rq_on(void);

// Queue items (the game's threads). Item numbers count up from 1; a fence refers to the item that holds it.
uint64_t rq_submit(uint64_t handle);                     // a recorded command list (nvnQueueSubmitCommands)
void rq_present(NvnTexture *t, uint64_t frame, bool advance);  // show t (the present pass, overlay, shots); advance:
                                                              // a new frame (false: backend_represent's repeat)
void rq_destroy_texture(NvnTexture *t);                  // backend_texture_destroyed + free(t), in queue order
enum { RQ_UPLOAD_RGBA, RQ_UPLOAD_NV12 };
void rq_upload(NvnTexture *t, int kind, void *data, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off);  // takes data

// Waiting (any thread): everything up to item q is translated and submitted to the GPU; the returned value is the
// backend's submission number to wait on for the GPU side (renderer_wait_seq).
uint64_t rq_wait_item(uint64_t q);
uint64_t rq_fence_seq(void);  // enqueue an ordered fence marker; its pending work is submitted before completion publication
// (LARECOMP_LOG_PACE) item q's place: items queued after it, not translated yet; true: in the frame being recorded
bool rq_fence_info(uint64_t q, uint64_t *behind, bool *untranslated);
uint64_t rq_current_item(void);  // on the render thread: the item being run; elsewhere 0
// On the render thread: the number of the frame whose work it runs (the presents it has done), as the game thread's
// frame counter was while that work was submitted - the game thread is up to a frame ahead by then. The backends'
// renderer_exec_frame() returns it there, so "once per frame" state in the translation (targets cleared at their
// first use in a frame, the widescreen composite, texture change checks) follows the frame being translated;
// renderer_present_count() stays the game thread's (the HLE hooks compare frames against it).
uint64_t rq_frame(void);
void rq_drain(void);   // every queued item done (QueueFinish; reboot)
void rq_free(void *p);  // free(p) in queue order (host objects queued lists may refer to)
void rq_call(void (*fn)(void *), void *arg);  // fn(arg) on the render thread, in queue order; returns when done
void rq_flush(void);   // drain, then the backend's full flush, on the render thread (nvnSyncWait with LARECOMP_SYNC_FLUSH)
// Quitting (the window closed, exit()): the render thread stops after the item it runs; true from then on (the
// backends present nothing more). backend_quit: rq_stop, the GPU idle, caches saved - before exit(); idempotent.
void rq_stop(void);
bool rq_stopping(void);
void backend_quit(void);

// (backends) called on the render thread
void backend_present_now(NvnTexture *t, uint64_t frame);
void backend_upload_now(NvnTexture *t, int kind, void *data, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off);
uint64_t renderer_submit_seq(void);  // the backend's latest GPU submission number (handed to the driver)
// ... the latest it queued: the Vulkan backend's recorder thread (record.c) may still hold the last ones; waited for by
// renderer_wait_seq, which first waits for the recorder (the Metal backend: the same as renderer_submit_seq)
uint64_t renderer_issued_seq(void);
void *backend_pool_push(void);       // (Metal) an autorelease pool around each item
void backend_pool_pop(void *p);

// (hang dump, rqueue.c) this thread is about to wait on `what` (a static string; arg: a number to show with it),
// rq_watch(NULL, 0) when done. Cheap: a few stores into the thread's own slot.
void rq_watch(const char *what, uint64_t arg);
// arms the hang dump's renderer checks (kernel.c's "[hang]"): nvnDeviceInitialize; once (the render thread's start too)
void rq_hang_watch_start(void);

// (pace.c, LARECOMP_LOG_PACE) per thread, since start: time in rq_wait_item / rq_drain (the game thread waiting for
// translation), waiting for the queue's lock, and the frontend's split of the presenting thread's frame (objects.c,
// the backends' present): in nvnQueueSubmitCommands, in GPU waits (nvnSyncWait / QueueFinish after the queue
// part), in backend_present (without the per-frame hooks), in the per-frame hooks.
extern __thread uint64_t g_rq_wait_ns, g_rq_lock_ns, g_submit_ns, g_gpu_wait_ns, g_present_ns, g_hooks_ns;

uint64_t rq_submit_leased(uint64_t handle,NvnClLease *lease);
bool rq_command_readers_stopped(void);
