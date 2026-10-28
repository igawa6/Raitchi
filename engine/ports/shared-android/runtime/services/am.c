/* Shared Android service overlay, derived from pinned LAS Recomp Android (MIT).
 * Copyright (c) 2026 Peiikai. See native/runtime/LICENSE-LASRecomp.
 * Required generic deltas adapted from Let's Go Recomp (MIT), copyright (c) 2026 raulzito12345rrs;
 * see native/runtime/LICENSE-LetsGo. */
// Applet manager (appletOE), apm, and lm.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../settings.h"
#include "../online_guard.h"
#include "title_adapter.h"

#define AM_NO_MESSAGE MAKE_RESULT(128, 3)
#define AM_NO_DATA MAKE_RESULT(128, 2)

static uint32_t msg_event_r, msg_event_w;
static KEvent *msg_event;
static uint32_t msg_queue[16];
static int msg_count;

static bool preselected_popped;
RT_ON_RESET(am_reset) {  // (reboot: the message queue starts over with FocusStateChanged)
    msg_event = NULL;
    msg_event_r = msg_event_w = 0;
    msg_count = 0;
    preselected_popped = false;
}

static void am_init_once(void) {
    if (msg_event) return;
    msg_event = kernel_create_event_pair(&msg_event_w, &msg_event_r);
    msg_queue[msg_count++] = 15;  // FocusStateChanged
    pthread_mutex_lock(&g_klock);
    kernel_signal(msg_event);
    pthread_mutex_unlock(&g_klock);
}

static uint32_t new_event(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

#define UNHANDLED(iface) LOG("[am] %s cmd %u (unhandled, ok)\n", iface, req->cmd)

static void common_state_getter(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = msg_event_r; break;  // GetEventHandle
    case 1:  // ReceiveMessage
        pthread_mutex_lock(&g_klock);
        if (msg_count) {
            OUT(uint32_t, 0) = msg_queue[0];
            memmove(msg_queue, msg_queue + 1, --msg_count * sizeof(uint32_t));
            if (!msg_count) msg_event->signaled = false;
            resp_size(resp, 4);
        } else {
            resp->result = AM_NO_MESSAGE;
        }
        pthread_mutex_unlock(&g_klock);
        break;
    case 5: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;   // GetOperationMode: docked
    case 6: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;  // GetPerformanceMode: boost
    case 8: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;   // GetBootMode
    case 9: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;   // GetCurrentFocusState: in focus
    case 13: case 61: resp->copy_handles[resp->ncopy++] = new_event(); break;
    case 60: {  // GetDefaultDisplayResolution
        int w = getenv("LARECOMP_DISPLAY_W") ? atoi(getenv("LARECOMP_DISPLAY_W")) : 1920;
        LOG("[am] GetDefaultDisplayResolution -> %dx1080\n", w);
        OUT(int32_t, 0) = w; OUT(int32_t, 4) = 1080; resp_size(resp, 8); break;
    }
    default: UNHANDLED("ICommonStateGetter");
    }
}

static void self_controller(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: LOG("[am] application requested exit\n"); exit(0);
    case 9: resp->copy_handles[resp->ncopy++] = new_event(); break;
    case 40: OUT(uint64_t, 0) = 1; resp_size(resp, 8); break;  // CreateManagedDisplayLayer
    case 69: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    default: UNHANDLED("ISelfController");
    }
}

static void window_controller(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: OUT(uint64_t, 0) = 0x1234; resp_size(resp, 8); break;  // GetAppletResourceUserId
    default: UNHANDLED("IWindowController");
    }
}

static void generic_ok(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[am] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

// ------------------------------------------------------------------ IStorage / IStorageAccessor
typedef struct { uint8_t *data; uint64_t size; } AmStorage;

static void storage_accessor(Service *s, IpcReq *req, IpcResp *resp) {
    AmStorage *st = s->state;
    switch (req->cmd) {
    case 0: memcpy(resp->data, &st->size, sizeof st->size); resp_size(resp, 8); break;  // GetSize
    case 10: {  // Write(offset, A)
        int64_t off = IN(int64_t, 0);
        IpcBuf *a = in_buf(req, 0);
        if (!a || off < 0 || (uint64_t)off > st->size) break;  // (a negative/OOB offset would underflow the clamp below)
        uint64_t n = a->size;
        if ((uint64_t)off + n > st->size) n = st->size - (uint64_t)off;
        memcpy(st->data + off, (void *)(uintptr_t)a->addr, n);
        break;
    }
    case 11: {  // Read(offset, B)
        int64_t off = IN(int64_t, 0);
        IpcBuf *b = out_buf(req, 0);
        if (!b || off < 0 || (uint64_t)off > st->size) break;  // (a negative/OOB offset would underflow the clamp below)
        uint64_t n = b->size;
        if ((uint64_t)off + n > st->size) n = st->size - (uint64_t)off;
        memcpy((void *)(uintptr_t)b->addr, st->data + off, n);
        break;
    }
    default: LOG("[am] IStorageAccessor cmd %u (ok)\n", req->cmd);
    }
}

static void am_storage(Service *s, IpcReq *req, IpcResp *resp) {
    if (req->cmd == 0) ipc_return_object(resp, service_new("IStorageAccessor", storage_accessor, s->state));
    else LOG("[am] IStorage cmd %u (ok)\n", req->cmd);
}

static Service *new_am_storage(const void *data, uint64_t size) {
    AmStorage *st = calloc(1, sizeof *st);
    if (!st) return NULL;
    st->data = calloc(1, size ? size : 1);
    if (!st->data) { free(st); return NULL; }
    st->size = size;
    if (data) memcpy(st->data, data, size);
    return service_new("IStorage", am_storage, st);
}

extern const uint8_t g_user_id[16];

// Library applets needed to decline online setup safely. Creation/storage/start
// have normal object semantics; cancellation is delivered through the applet's
// result protocol, not a fabricated successful login or an absent accessor.
typedef struct {
    pthread_mutex_t lock;
    uint32_t id, readable[3], writable[3];
    KEvent *events[3];
    bool completed, output_popped, profile_local;
} OfflineApplet;
static void offline_applet_close(Service *s) {
    OfflineApplet *a = s->state;
    if (a) {
        for (unsigned i = 0; i < 3; ++i) {
            kernel_close_service_handle(a->writable[i], a->events[i]);
            kernel_close_service_handle(a->readable[i], a->events[i]);
        }
        pthread_mutex_destroy(&a->lock); free(a); s->state = NULL;
    }
}
static uint32_t offline_applet_event(OfflineApplet *a, unsigned index) {
    if (!a->events[index]) {
        a->events[index] = kernel_create_event_pair(&a->writable[index], &a->readable[index]);
    }
    if (a->completed && index != 2) {
        pthread_mutex_lock(&g_klock); kernel_signal(a->events[index]); pthread_mutex_unlock(&g_klock);
    }
    return kernel_new_handle(a->events[index]);
}
static void offline_applet_accessor(Service *s, IpcReq *req, IpcResp *resp) {
    OfflineApplet *a = s->state;
    pthread_mutex_lock(&a->lock);
    switch (req->cmd) {
    case 0: case 105: case 106: { // completion / output / interactive-output events
        unsigned index = req->cmd == 0 ? 0 : req->cmd == 105 ? 1 : 2;
        resp->copy_handles[resp->ncopy++] = offline_applet_event(a, index); break;
    }
    case 1: OUT(uint8_t, 0) = a->completed; resp_size(resp, 1); break;
    case 10: case 20: case 25: // Start / RequestExit / Terminate finish without online access.
        a->completed = true;
        pthread_mutex_lock(&g_klock);
        for (unsigned i = 0; i < 2; ++i) if (a->events[i]) kernel_signal(a->events[i]);
        pthread_mutex_unlock(&g_klock);
        LOG("[offline] library applet %#x completed without an online account/connection\n", a->id);
        break;
    case 30: // GetResult: profile cancellation lives in UiReturnArg, as on hardware.
        if (!a->completed) resp->result = MAKE_RESULT(1, 125);
        else if (a->id == 0x0f) resp->result = RAITCHI_OFFLINE_NETWORK_DISABLED;
        break;
    case 50: case 60: // focus/GPU scheduling configuration does not enable online access.
        break;
    case 100: { // PushInData: consume the caller's resolved IStorage synchronously.
        Service *input = req->ninput_objects ? req->input_objects[0] : NULL;
        if (!input || strcmp(input->name, "IStorage") || !input->state) {
            resp->result = MAKE_RESULT(128, 518); break;
        }
        AmStorage *storage = input->state;
        if (a->id == 0x10 && storage->size != 0x20) {
            // ProfileSelect v1/v2/v3 UiSettings: mode, eight invalid UIDs,
            // application ID, then display flags at0x90. Only a local
            // UserSelector can select our existing local profile. A network
            // requirement, editing/creation mode or malformed configuration
            // cannot turn it into a linked Nintendo account.
            a->profile_local = false;
            if ((storage->size != 0x98 && storage->size != 0xa0) || !storage->data) {
                resp->result = MAKE_RESULT(128, 500); break;
            }
            uint32_t mode; memcpy(&mode, storage->data, sizeof mode);
            bool invalid = false;
            for (unsigned i = 0; i < 8; ++i)
                if (!memcmp(storage->data + 8 + 16 * i, g_user_id, 16)) invalid = true;
            a->profile_local = mode == 0 && storage->data[0x90] == 0 && !invalid;
        }
        break;
    }
    case 101: // PopOutData for ProfileSelect: cancelled UiReturnArg {u64 result, Uid zero}.
        if (!a->completed || a->id != 0x10 || a->output_popped) { resp->result = AM_NO_DATA; break; }
        {
            uint8_t output[0x18] = {0};
            uint64_t result = a->profile_local ? 0 : RAITCHI_OFFLINE_ACCOUNT_CANCELLED;
            memcpy(output, &result, sizeof result);
            if (a->profile_local) memcpy(output + 8, g_user_id, 16);
            Service *storage = new_am_storage(output, sizeof output);
            if (!storage) { resp->result = MAKE_RESULT(1, 104); break; }
            ipc_return_object(resp, storage);
            a->output_popped = true;
        }
        break;
    case 104: resp->result = AM_NO_DATA; break;
    case 110: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break; // no process exit required
    default:
        resp->result = RAITCHI_OFFLINE_UNSUPPORTED;
        LOG("[offline] library applet accessor cmd %u unsupported\n", req->cmd);
    }
    pthread_mutex_unlock(&a->lock);
}
static void library_applet_creator(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 10) { // CreateStorage(size), used for applet common/config input.
        int64_t size = -1;
        if (req->data_size >= sizeof size) memcpy(&size, req->data, sizeof size);
        if (size < 0 || size > 1024 * 1024) { resp->result = MAKE_RESULT(128, 506); return; }
        Service *storage = new_am_storage(NULL, (uint64_t)size);
        if (!storage) resp->result = MAKE_RESULT(1, 104);
        else ipc_return_object(resp, storage);
        return;
    }
    if (req->cmd == 2) { OUT(uint8_t, 0) = 0; resp_size(resp, 1); return; }
    if (req->cmd == 0 || req->cmd == 3) { // CreateLibraryApplet / Ex
        uint32_t id = 0;
        if (req->data_size >= 8) memcpy(&id, req->data, sizeof id);
        // Error merely acknowledges an error; ProfileSelect returns a cancelled
        // empty UID; NetConnect fails offline. Other applet protocols stay
        // explicit unsupported errors, never success with a missing object.
        if (id != 0x0e && id != 0x0f && id != 0x10) {
            resp->result = RAITCHI_OFFLINE_UNSUPPORTED;
            LOG("[offline] library applet %#x unsupported\n", id); return;
        }
        OfflineApplet *a = calloc(1, sizeof *a);
        if (!a) { resp->result = MAKE_RESULT(1, 104); return; }
        if (pthread_mutex_init(&a->lock, NULL)) { free(a); resp->result = MAKE_RESULT(1, 104); return; }
        a->id = id;
        Service *accessor = service_new("ILibraryAppletAccessor", offline_applet_accessor, a);
        accessor->on_close = offline_applet_close;
        ipc_return_object(resp, accessor);
        return;
    }
    resp->result = RAITCHI_OFFLINE_UNSUPPORTED;
}

static void application_functions(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1:  // PopLaunchParameter(kind)
        if (IN(uint32_t, 0) == 2 && !preselected_popped) {  // PreselectedUser
            uint8_t p[0x88] = {0};
            *(uint32_t *)p = 0xC79497CA;
            *(uint32_t *)(p + 4) = 1;
            memcpy(p + 8, g_user_id, 16);
            preselected_popped = true;
            ipc_return_object(resp, new_am_storage(p, sizeof p));
        } else {
            resp->result = AM_NO_DATA;
        }
        break;
    case 20: OUT(uint64_t, 0) = 0; resp_size(resp, 8); break;  // EnsureSaveData
    case 21: {  // GetDesiredLanguage: the game's text language (settings: runtime/settings.c, "language")
        char lang[8] = {0};
        snprintf(lang, sizeof lang, "%s", settings_language());
        memcpy(resp->data, lang, 8);
        resp_size(resp, 8);
        break;
    }
    case 23: {  // GetDisplayVersion: accepted build policy, never a title constant in the service.
        const TitleAdapter *adapter = title_adapter_selected();
        if (!adapter || !adapter->version) { resp->result = MAKE_RESULT(1, 123); break; }
        memset(resp->data, 0, 0x10);
        snprintf((char *)resp->data, 0x10, "%s", adapter->version);
        resp_size(resp, 0x10);
        break;
    }
    case 40: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // NotifyRunning
    case 130: case 140: resp->copy_handles[resp->ncopy++] = new_event(); break;
    default: UNHANDLED("IApplicationFunctions");
    }
}

static void application_proxy(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    static const struct { int cmd; const char *name; ServiceHandler h; } subs[] = {
        {0, "ICommonStateGetter", common_state_getter},
        {1, "ISelfController", self_controller},
        {2, "IWindowController", window_controller},
        {3, "IAudioController", generic_ok},
        {4, "IDisplayController", generic_ok},
        {10, "IProcessWindingController", generic_ok},
        {11, "ILibraryAppletCreator", library_applet_creator},
        {20, "IApplicationFunctions", application_functions},
        {1000, "IDebugFunctions", generic_ok},
    };
    for (unsigned i = 0; i < sizeof subs / sizeof *subs; i++) {
        if (subs[i].cmd == (int)req->cmd) {
            ipc_return_object(resp, service_new(subs[i].name, subs[i].h, NULL));
            return;
        }
    }
    UNHANDLED("IApplicationProxy");
}

SERVICE("appletOE", applet_oe) {
    (void)s;
    am_init_once();
    if (req->cmd == 0) ipc_return_object(resp, service_new("IApplicationProxy", application_proxy, NULL));
    else UNHANDLED("appletOE");
}

// ------------------------------------------------------------------ apm
static void apm_session(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 1) { OUT(uint32_t, 0) = 0x92220008; resp_size(resp, 4); }  // GetPerformanceConfiguration
}

SERVICE("apm", apm) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("apm:ISession", apm_session, NULL));
    else if (req->cmd == 1) { OUT(uint32_t, 0) = 1; resp_size(resp, 4); }
}

// ------------------------------------------------------------------ lm (guest log output)
static void lm_logger(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s; (void)resp;
    if (req->cmd != 0) return;
    IpcBuf *b = in_buf(req, 0);
    if (!b || !b->size) return;
    // Log packets are TLV chunks after a 0x18-byte header; print the string chunks.
    const uint8_t *p = (const uint8_t *)(uintptr_t)b->addr, *end = p + b->size;
    p += 0x18;
    char line[1024];
    int n = 0;
    while (p + 2 <= end) {
        uint8_t key = p[0], len = p[1];
        p += 2;
        if (p + len > end) break;
        if (key == 2 || key == 4 || key == 5 || key == 6 || key == 7) {  // message, file, function, module, thread
            n += snprintf(line + n, sizeof line - n, "%s%.*s", n ? " | " : "", len, (const char *)p);
            if (n >= (int)sizeof line) break;
        }
        p += len;
    }
    if (n) LOG("[lm] %s\n", line);
}

SERVICE("lm", lm) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("lm:ILogger", lm_logger, NULL));
}
