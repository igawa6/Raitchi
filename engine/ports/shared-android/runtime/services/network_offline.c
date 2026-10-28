/* Offline NIFM protocol implementation. No sockets, host network discovery,
 * credentials or online service capability are provided. Interface command
 * identities follow the public NIFM IPC protocol and pinned Eden source audit. */
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../hle.h"
#include "../ipc.h"
#include "../online_guard.h"

typedef struct {
    pthread_mutex_t lock;
    KEvent *events[2];
    uint32_t readable[2], writable[2];
    bool submitted;
} OfflineNetworkRequest;

static void network_request_close(Service *s) {
    OfflineNetworkRequest *state = s->state;
    if (state) {
        for (unsigned i = 0; i < 2; ++i) {
            kernel_close_service_handle(state->writable[i], state->events[i]);
            kernel_close_service_handle(state->readable[i], state->events[i]);
        }
        pthread_mutex_destroy(&state->lock); free(state); s->state = NULL;
    }
}
static void network_signal_locked(OfflineNetworkRequest *state) {
    pthread_mutex_lock(&g_klock);
    for (unsigned i = 0; i < 2; ++i) if (state->events[i]) kernel_signal(state->events[i]);
    pthread_mutex_unlock(&g_klock);
}
static void network_request(Service *s, IpcReq *req, IpcResp *resp) {
    OfflineNetworkRequest *state = s->state;
    switch (req->cmd) {
    case 0: // GetRequestState: 1 is NotSubmitted/Invalid; never Accepted (3).
        OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;
    case 1: // GetResult: terminal offline failure, never a connection success.
    case 21: // GetAppletInfo: no network setup applet is available to enable it.
        resp->result = RAITCHI_OFFLINE_NETWORK_DISABLED; break;
    case 2: // GetSystemEventReadableHandles (state changed/completed)
        pthread_mutex_lock(&state->lock);
        for (unsigned i = 0; i < 2; ++i) {
            if (!state->events[i]) {
                state->events[i] = kernel_create_event_pair(&state->writable[i], &state->readable[i]);
            }
            // IPC copy responses use an independent guest handle. The hidden
            // base pair belongs to this service and is released on close.
            resp->copy_handles[resp->ncopy++] = kernel_new_handle(state->events[i]);
        }
        if (state->submitted) network_signal_locked(state);
        pthread_mutex_unlock(&state->lock);
        break;
    case 3: // Cancel
    case 4: // Submit: request accepted for processing, immediately fails offline.
        pthread_mutex_lock(&state->lock);
        state->submitted = true;
        network_signal_locked(state);
        pthread_mutex_unlock(&state->lock);
        LOG("[offline] NIFM request %s: network communication disabled\n", req->cmd == 4 ? "submitted" : "cancelled");
        break;
    case 5: case 6: case 8: case 9: case 10: case 11: case 12: case 13:
    case 14: case 15: case 16: case 17: case 18: case 23:
        // Request configuration can be accepted without enabling networking.
        break;
    default:
        resp->result = RAITCHI_OFFLINE_UNSUPPORTED;
        LOG("[offline] NIFM request cmd %u unsupported\n", req->cmd);
    }
}
static void network_general(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: { // GetClientId: local NIFM interface identity, not an online account.
        const uint64_t client = 1; memcpy(resp->data, &client, sizeof client); resp_size(resp, 8); break;
    }
    case 4: { // CreateRequest
        OfflineNetworkRequest *state = calloc(1, sizeof *state);
        if (!state) { resp->result = MAKE_RESULT(1, 104); break; }
        if (pthread_mutex_init(&state->lock, NULL)) { free(state); resp->result = MAKE_RESULT(1, 104); break; }
        Service *request = service_new("nifm:IRequest", network_request, state);
        request->on_close = network_request_close;
        ipc_return_object(resp, request);
        break;
    }
    case 17: case 20: case 21: case 22: case 44: // enabled/accepted/available queries
        OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 18: // GetInternetConnectionStatus: invalid interface, no bars, not connected.
        memset(resp->data, 0, 3); resp_size(resp, 3); break;
    case 33: case 34: // System service is present; request policy remains offline.
        break;
    default:
        resp->result = RAITCHI_OFFLINE_NETWORK_DISABLED;
        LOG("[offline] NIFM general cmd %u denied: network communication disabled\n", req->cmd);
    }
}
static void network_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 4 || req->cmd == 5)
        ipc_return_object(resp, service_new("nifm:IGeneralService", network_general, NULL));
    else resp->result = RAITCHI_OFFLINE_UNSUPPORTED;
}
SERVICE("nifm:u", nifm_u) { network_root(s, req, resp); }
SERVICE("nifm:s", nifm_s) { network_root(s, req, resp); }
SERVICE("nifm:a", nifm_a) { network_root(s, req, resp); }
