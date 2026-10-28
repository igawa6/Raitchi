/* Shared offline account overlay from LASRecomp Android (MIT), copyright
 * (c) 2026 Peiikai. See native/runtime/LICENSE-LASRecomp. Local user/save
 * identity is preserved; no Nintendo account ID or online token is fabricated. */
// acc:u0 (one local user) and the preselected-user launch parameter.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../online_guard.h"

const uint8_t g_user_id[16] = {0x4c, 0x41, 0x52, 0x65, 0x63, 0x6f, 0x6d, 0x70, 0x01, 0, 0, 0, 0, 0, 0, 0x01};

static void write_uid_list(IpcReq *req) {
    IpcBuf *c = out_buf(req, 0);
    if (!c) return;
    memset((void *)(uintptr_t)c->addr, 0, c->size);
    if (c->size >= 16) memcpy((void *)(uintptr_t)c->addr, g_user_id, 16);
}

static void profile(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 1: {  // Get / GetBase -> ProfileBase {uid, u64 timestamp, nickname[0x21]}
        memset(resp->data, 0, 0x38);
        memcpy(resp->data, g_user_id, 16);
        const uint64_t timestamp = 1600000000;
        memcpy(resp->data + 16, &timestamp, sizeof timestamp);
        strcpy((char *)resp->data + 24, "Link");
        resp_size(resp, 0x38);
        if (req->cmd == 0 && out_buf(req, 0)) memset((void *)(uintptr_t)out_buf(req, 0)->addr, 0, out_buf(req, 0)->size);
        break;
    }
    case 10: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetImageSize
    case 11: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // LoadImage
    default: LOG("[acc] IProfile cmd %u (ok)\n", req->cmd);
    }
}

static void manager(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    // A local profile is usable for saves, but is not a linked Nintendo account.
    // Failed calls return no ID/token/object, so SDKs cannot use a fake success.
    resp->result = req->cmd == 1 ? RAITCHI_OFFLINE_ACCOUNT_UNLINKED : RAITCHI_OFFLINE_ACCOUNT_REQUEST;
    LOG("[offline] account manager cmd %u denied: online account unavailable\n", req->cmd);
}

SERVICE("acc:u0", acc_u0) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(int32_t, 0) = 1; resp_size(resp, 4); break;  // GetUserCount
    case 1:
        if (req->data_size < 16) { resp->result = MAKE_RESULT(1, 14); break; }
        OUT(uint8_t, 0) = !memcmp(req->data, g_user_id, 16); resp_size(resp, 1); break;
    case 2: case 3: case 141: write_uid_list(req); break;  // ListAllUsers / ListOpenUsers / ListQualifiedUsers
    case 4: memcpy(resp->data, g_user_id, 16); resp_size(resp, 16); break;  // GetLastOpenedUser
    case 51: case 52: { // TrySelectUserWithoutInteraction(network account required)
        if (!req->data_size) { resp->result = MAKE_RESULT(1, 14); break; }
        if (req->data[0]) { resp->result = RAITCHI_OFFLINE_ACCOUNT_REQUEST; break; }
        memcpy(resp->data, g_user_id, 16); resp_size(resp, 16); break;
    }
    case 5: ipc_return_object(resp, service_new("IProfile", profile, NULL)); break;
    case 50: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 100: case 140: case 160: break;  // InitializeApplicationInfo
    case 101: ipc_return_object(resp, service_new("IManagerForApplication", manager, NULL)); break;
    case 102: case 103: case 120: // Online authentication/availability/guest login.
        resp->result = RAITCHI_OFFLINE_ACCOUNT_REQUEST;
        LOG("[offline] account cmd %u denied: online account unavailable\n", req->cmd);
        break;
    case 110: case 111: break; // Optional local save-thumbnail notifications.
    case 150: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // IsUserAccountSwitchLocked
    default:
        // Unknown account extensions must not claim successful authentication
        // or return a missing async context. Preserve the supported local APIs.
        resp->result = RAITCHI_OFFLINE_UNSUPPORTED;
        LOG("[offline] account cmd %u unsupported\n", req->cmd);
    }
}
