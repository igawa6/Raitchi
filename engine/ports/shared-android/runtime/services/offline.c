/* Adapted from Let's Go Recomp (MIT), copyright (c) 2026 raulzito12345rrs.
 * See native/runtime/LICENSE-LetsGo and LICENSE-LASRecomp. */
// Offline compatibility responses only: Bluetooth hardware, pairing and communication are NOT implemented.
// The CSRNG handler below supplies actual host random bytes. Bluetooth object/event shapes preserve the
// working reference, but no event ever discovers a device and unknown commands explicitly fail.
// System services for features this port runs without - Bluetooth (Poke Ball Plus, Pokemon GO link) and
// the hardware random generator. Each answers the way a console with nothing paired / nothing nearby would, and
// logs what it was asked, so a feature that needs more shows up in the log.
#include <string.h>
#include <stdlib.h>

#include "../hle.h"
#include "../ipc.h"

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ btm:u (nn::btm::user, BLE for the Poke Ball Plus)
// IBtmUserCore (switchbrew "BTM services"): the Acquire*Event commands return a bool and an event the game waits on;
// the events are never signalled (no BLE device is ever found or connected).
static void btm_user_core(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0:   // AcquireBleScanEvent
    case 17:  // AcquireBleConnectionEvent
    case 21:  // AcquireBlePairingEvent (the GO Park settings wait on it: without it they abort, issue #1)
    case 26:  // AcquireBleServiceDiscoveryEvent
    case 33:  // AcquireBleMtuConfigEvent
        OUT(uint8_t, 0) = 1;
        resp_size(resp, 4);
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        break;
    default:  // (no output, like a console with nothing paired: a made-up 0x10-byte reply made the GO settings abort, #1)
        LOG("[btm] IBtmUserCore cmd %u unsupported (no BLE implementation)\n", req->cmd);
        resp->result = MAKE_RESULT(1, 123);
    }
}

SERVICE("btm:u", btm_u) {
    (void)s;
    if (req->cmd == 0) {  // GetCore
        ipc_return_object(resp, service_new("IBtmUserCore", btm_user_core, NULL));
        return;
    }
    LOG("[btm] btm:u cmd %u unsupported (no BLE implementation)\n", req->cmd);
    resp->result = MAKE_RESULT(1, 123);
}

// ------------------------------------------------------------------ bt (nn::bluetooth user)
SERVICE("bt", bt) {
    (void)s;
    switch (req->cmd) {
    case 0:  // RegisterBleEvent
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        break;
    case 9:  // Observed reference compatibility reply; meaning/feature behavior remains unverified.
        LOG("[bt] cmd 9 offline placeholder (no Bluetooth implementation)\n");
        memset(resp->data, 0, 0x10); resp_size(resp, 0x10);
        break;
    default:
        LOG("[bt] cmd %u unsupported (no Bluetooth implementation)\n", req->cmd);
        resp->result = MAKE_RESULT(1, 123);
    }
}

// ------------------------------------------------------------------ csrng (spl random bytes)
SERVICE("csrng", csrng) {
    (void)s;
    if (req->cmd == 0) {  // GenerateRandomBytes(out buffer)
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size) {
            uint8_t *p = (uint8_t *)(uintptr_t)b->addr;
            for (uint64_t i = 0; i < b->size; i += 4) {
                uint32_t v = arc4random();
                memcpy(p + i, &v, b->size - i < 4 ? b->size - i : 4);
            }
        }
        return;
    }
    LOG("[csrng] cmd %u unsupported\n", req->cmd);
    resp->result = MAKE_RESULT(1, 123);
}
