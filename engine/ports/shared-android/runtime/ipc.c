/* Shared Android IPC overlay. Base: LASRecomp Android cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * TIPC codec adapted from EoWRecomp dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505 (runtime/ipc.c).
 * MIT License
 *
 * Copyright (c) 2026 Peiikai
 * Copyright (c) 2026 EoWRecomp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
// IPC message parsing/serialization and session management.
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "ipc.h"

#ifdef __APPLE__
extern const ServiceDef __start_services[] __asm__("section$start$__DATA$__services");
extern const ServiceDef __stop_services[] __asm__("section$end$__DATA$__services");
#elif defined(__linux__)
extern const ServiceDef __start_services[], __stop_services[];  // (ELF: defined by the linker)
#else
// PE/COFF: .svc$a < .svc$m (entries, ipc.h) < .svc$z (the linker sorts by the text after '$')
__attribute__((section(".svc$a"), used)) static const ServiceDef __start_services[1];
__attribute__((section(".svc$z"), used)) static const ServiceDef __stop_services[1];
#endif

static void stub_handler(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[ipc] STUB %s cmd %u (in %u bytes, x%d a%d b%d c%d) -> ok\n", s->name, req->cmd, req->data_size, req->nx,
        req->na, req->nb, req->nc);
    resp->result = 0;
    resp_size(resp, 0x40);  // zeroed outputs
}

Service *service_new(const char *name, ServiceHandler h, void *state) {
    Service *s = calloc(1, sizeof *s);
    s->name = strdup(name);
    s->handler = h;
    s->state = state;
    return s;
}

Service *service_lookup(const char *name) {
    const ServiceDef *start = __start_services, *stop = __stop_services;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const ServiceDef *d = start; d < stop; d++)
        if (d->name && !strcmp(d->name, name)) return service_new(name, d->handler, NULL);
    LOG("[ipc] no HLE implementation for service '%s', using logging stub\n", name);
    return service_new(name, stub_handler, NULL);
}

void ipc_return_object(IpcResp *resp, Service *s) { resp->objects[resp->nobjects++] = s; }

static uint32_t session_new(Service *s) {
    KSession *k = calloc(1, sizeof *k);
    k->obj.type = KOBJ_SESSION;
    k->svc = s;
    s->refs++;
    return kernel_new_handle(&k->obj);
}

// Resolve borrowed input service objects before dispatch. Domain object IDs are
// scoped to the caller's domain; moved session handles resolve through the
// kernel table. Other moved object kinds remain raw handles, never cast to a
// service. Handlers that require storage validate the resolved type explicitly.
static void ipc_input_services(IpcReq *req, KSession *session, bool domain) {
    req->ninput_objects = domain ? req->ndomain_in : req->nmove;
    if (req->ninput_objects > 8) req->ninput_objects = 8;
    for (int i = 0; i < req->ninput_objects; ++i) {
        if (domain) {
            uint32_t id = req->domain_in_objs[i];
            req->input_objects[i] = session->domain && id < 256 ? session->domain->objs[id] : NULL;
        } else {
            KObject *object = handle_get(req->move_handles[i]);
            req->input_objects[i] = object && object->type == KOBJ_SESSION ? ((KSession *)object)->svc : NULL;
        }
    }
}

static void service_release(Service *s) {
    if (s && --s->refs == 0 && s->on_close) s->on_close(s);
}

uint32_t ipc_connect_named_port(const char *name) {
    if (!strcmp(name, "sm:")) return session_new(service_lookup("sm:"));
    LOG("[ipc] ConnectToNamedPort('%s') unsupported\n", name);
    return 0;
}

// ------------------------------------------------------------------ sm:
SERVICE("sm:", sm_handler) {
    (void)s;
    switch (req->cmd) {
    case 0:  // RegisterClient
        break;
    case 1: {  // GetServiceHandle(name)
        char name[9] = {0};
        memcpy(name, req->data, 8);
        LOG("[sm] GetService %s\n", name);
        ipc_return_object(resp, service_lookup(name));
        break;
    }
    default:
        LOG("[sm] unknown cmd %u\n", req->cmd);
    }
}

// ------------------------------------------------------------------ message codec
static uint64_t buf_addr(const uint32_t *w) {
    return w[1] | ((uint64_t)(w[2] >> 28) << 32) | ((uint64_t)((w[2] >> 2) & 7) << 36);
}

// TIPC (the "tiny IPC" Horizon 12 added; SDK 12+ talks to sm: with it): the message type is 16 + the command id,
// 15 closes the session; the raw data words follow the header directly (no SFCI / SFCO header, no padding, no
// domains). The reply: result in the first raw word, then the output data; handles in the special header.
static int tipc_request(Cpu *c, KSession *sess, uint32_t handle, IpcReq *req, uint32_t *raw, uint32_t raw_words) {
    uint32_t *msg = (uint32_t *)(uintptr_t)c->tpidrro;
    if (req->type == 15) {
        LOG("[ipc] close session %s (handle %x, tipc)\n", sess->svc->name, handle);
        service_release(sess->svc);
        return 0;
    }
    IpcResp resp = {0};
    req->cmd = req->type - 16;
    req->data = (const uint8_t *)raw;
    req->data_size = raw_words * 4;
    ipc_input_services(req, sess, false);
    sess->svc->handler(sess->svc, req, &resp);
    for (int i = 0; i < resp.nobjects; i++) resp.move_handles[resp.nmove++] = session_new(resp.objects[i]);
    memset(msg, 0, 0x100);
    uint32_t data_words = 1 + (resp.data_size + 3) / 4;
    int has_handles = resp.ncopy || resp.nmove;
    msg[0] = req->type;
    msg[1] = data_words | (has_handles ? 0x80000000u : 0);
    uint32_t *q = msg + 2;
    if (has_handles) {
        *q++ = (resp.ncopy << 1) | (resp.nmove << 5);
        for (int i = 0; i < resp.ncopy; i++) *q++ = resp.copy_handles[i];
        for (int i = 0; i < resp.nmove; i++) *q++ = resp.move_handles[i];
    }
    q[0] = resp.result;
    memcpy(q + 1, resp.data, resp.data_size);
    return 0;
}

int ipc_handle_request(Cpu *c, uint32_t handle) {
    KObject *o = handle_get(handle);
    if (!o || o->type != KOBJ_SESSION) {
        LOG("[ipc] SendSyncRequest on bad handle %x\n", handle);
        return 0xE401;
    }
    KSession *sess = (KSession *)o;
    uint32_t *msg = (uint32_t *)(uintptr_t)c->tpidrro;
    IpcReq req = {0};
    IpcResp resp = {0};
    req.type = msg[0] & 0xFFFF;
    int nx = (msg[0] >> 16) & 0xF, na = (msg[0] >> 20) & 0xF, nb = (msg[0] >> 24) & 0xF;
    uint32_t raw_words = msg[1] & 0x3FF;
    uint32_t cflags = (msg[1] >> 10) & 0xF;
    uint32_t *p = msg + 2;
    uint32_t hd = 0;
    uint32_t ncopy = 0, nmove = 0;
    if (msg[1] & 0x80000000u) {
        hd = *p++;
        ncopy = (hd >> 1) & 0xF;
        nmove = (hd >> 5) & 0xF;
    }
    // Horizon's per-thread IPC command buffer is 0x100 bytes (the response path clears exactly
    // that range below). Validate the full wire layout before reading any variable-length fields:
    // otherwise even though the request arrays are clamped, a guest can make the descriptor/raw
    // pointers walk beyond the command buffer. `raw_words` is measured from raw; CMIF starts at
    // the next 16-byte boundary, so check both ends. C descriptors follow raw + raw_words.
    size_t word = (size_t)(p - msg);
    if (hd & 1) word += 2;
    word += ncopy + nmove + (size_t)nx * 2 + (size_t)(na + nb) * 3;
    uintptr_t raw_addr = (uintptr_t)msg + word * sizeof *msg;
    size_t aligned_word = (size_t)(((raw_addr + 15u) & ~(uintptr_t)15u) - (uintptr_t)msg) / 4;
    size_t raw_end = word + raw_words;
    // CMIF parsing reads SFCI and the command id even when raw_words is malformedly short.
    // For normal-sized CMIF payloads, req.data_size ends at aligned + raw_words - 4 words;
    // raw_end separately covers the wire raw area and the following C descriptors.
    size_t aligned_words = req.type >= 15 ? 0 : (raw_words > 8 ? (size_t)raw_words - 4 : 3);
    if (aligned_words < 3 && req.type < 15) aligned_words = 3;
    size_t aligned_end = aligned_word + aligned_words;
    size_t ncdesc = cflags < 2 ? 0 : (cflags == 2 ? 1 : cflags - 2);
    size_t cdesc_end = raw_end + ncdesc * 2;
    size_t end = cdesc_end > aligned_end ? cdesc_end : aligned_end;
    if (end > 0x100 / sizeof(uint32_t)) {
        LOG("[ipc] malformed request exceeds 0x100-byte command buffer\n");
        return 0xE401;
    }
    if (msg[1] & 0x80000000u) {
        if (hd & 1) { req.pid = p[0] | ((uint64_t)p[1] << 32); p += 2; }
        req.ncopy = ncopy;
        req.nmove = nmove;
        // ncopy/nmove are 4-bit wire fields (0..15) but IpcReq.copy_handles/move_handles are [8]
        // (ipc.h). Still consume every wire word so later fields parse at the right offset, but
        // never write past the fixed-size arrays.
        for (int i = 0; i < req.ncopy; i++) { uint32_t h = *p++; if (i < 8) req.copy_handles[i] = h; }
        for (int i = 0; i < req.nmove; i++) { uint32_t h = *p++; if (i < 8) req.move_handles[i] = h; }
        if (req.ncopy > 8) req.ncopy = 8;
        if (req.nmove > 8) req.nmove = 8;
    }
    // nx/na/nb are likewise 4-bit wire fields (0..15); IpcReq.x/a/b are [8] (ipc.h). Advance `p`
    // by the declared count either way, clamp what we store/report to the array capacity.
    for (int i = 0; i < nx; i++, p += 2) {
        if (i >= 8) continue;
        req.x[i].addr = p[1] | ((uint64_t)((p[0] >> 12) & 0xF) << 32) | ((uint64_t)((p[0] >> 6) & 0x7) << 36);
        req.x[i].size = p[0] >> 16;
    }
    req.nx = nx > 8 ? 8 : nx;
    for (int i = 0; i < na; i++, p += 3) {
        if (i >= 8) continue;
        req.a[i].size = p[0] | ((uint64_t)((p[2] >> 24) & 0xF) << 32);
        req.a[i].addr = buf_addr(p);
    }
    req.na = na > 8 ? 8 : na;
    for (int i = 0; i < nb; i++, p += 3) {
        if (i >= 8) continue;
        req.b[i].size = p[0] | ((uint64_t)((p[2] >> 24) & 0xF) << 32);
        req.b[i].addr = buf_addr(p);
    }
    req.nb = nb > 8 ? 8 : nb;
    uint32_t *raw = p;
    // raw data is aligned to 16 bytes relative to the message start
    uint32_t *aligned = (uint32_t *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    if (cflags >= 2) {
        uint32_t *cdesc = raw + raw_words;
        int ncd = cflags == 2 ? 1 : (int)cflags - 2;
        // cflags is a 4-bit wire field, so ncd can reach 13; IpcReq.c is [8] (ipc.h).
        for (int i = 0; i < ncd && i < 8; i++) {
            req.c[i].addr = cdesc[2 * i] | ((uint64_t)(cdesc[2 * i + 1] & 0xFFFF) << 32);
            req.c[i].size = cdesc[2 * i + 1] >> 16;
        }
        req.nc = ncd > 8 ? 8 : ncd;
    }

    if (req.type >= 15) return tipc_request(c, sess, handle, &req, raw, raw_words);
    if (req.type == 2) {  // close
        LOG("[ipc] close session %s (handle %x)\n", sess->svc->name, handle);
        if (!sess->domain) service_release(sess->svc);  // (a domain's objects stay: closed one by one)
        return 0;
    }
    uint8_t *payload = (uint8_t *)aligned;
    bool dom = false;  // response is domain-wrapped
    if (req.type == 4 || req.type == 6) {
        uint16_t domain_size = 0;
        if (sess->domain) {
            // The domain header carries its own payload length and object count. Keep those
            // secondary offsets inside the raw request area as well as inside the TLS buffer.
            if ((uintptr_t)payload + 4 > (uintptr_t)msg + 0x100) return 0xE401;
            uint8_t nin = payload[1];
            domain_size = *(uint16_t *)(payload + 2);
            size_t domain_bytes = 16u + domain_size + (size_t)nin * 4;
            if (payload[0] != 2 && domain_bytes < 28) domain_bytes = 28;  // SFCI plus command id follows the domain header.
            if (payload[0] == 2 && domain_bytes < 8) domain_bytes = 8;
            size_t domain_end = (size_t)((uintptr_t)payload - (uintptr_t)msg) + domain_bytes;
            if (domain_end > raw_end * sizeof(uint32_t) || domain_end > 0x100) {
                LOG("[ipc] malformed domain request exceeds raw command data\n");
                return 0xE401;
            }
            dom = true;
            uint8_t dcmd = payload[0];
            req.domain_obj = *(uint32_t *)(payload + 4);
            if (dcmd == 2) {  // close object
                if (req.domain_obj < 256) {
                    Service *o = sess->domain->objs[req.domain_obj];
                    sess->domain->objs[req.domain_obj] = NULL;
                    service_release(o);
                }
                goto write_response;
            }
            req.ndomain_in = nin > 8 ? 8 : nin;
            for (int i = 0; i < nin; i++) {
                uint32_t object_id = *(uint32_t *)(payload + 16 + domain_size + 4 * i);
                if (i < 8) req.domain_in_objs[i] = object_id;
            }
            payload += 16;
        }
        if (memcmp(payload, "SFCI", 4)) {
            LOG("[ipc] bad SFCI magic on %s (domain=%d):", sess->svc->name, sess->domain != NULL);
            for (int i = 0; i < 24; i++) LOG(" %08x", msg[i]);
            LOG("\n");
        }
        req.cmd = *(uint32_t *)(payload + 8);
        req.data = payload + 16;
        if (dom) {
            size_t declared = domain_size > 16 ? (size_t)domain_size - 16 : 0;
            uintptr_t wire_end = (uintptr_t)msg + raw_end * sizeof(uint32_t);
            uintptr_t data_start = (uintptr_t)req.data;
            size_t wire_bytes = wire_end > data_start ? (size_t)(wire_end - data_start) : 0;
            req.data_size = declared < wire_bytes ? (uint32_t)declared : (uint32_t)wire_bytes;
        } else {
            req.data_size = raw_words * 4 > 32 ? raw_words * 4 - 32 : 0;
        }
        Service *target = sess->svc;
        if (sess->domain) {
            target = req.domain_obj < 256 ? sess->domain->objs[req.domain_obj] : NULL;
            if (!target) { LOG("[ipc] domain object %u missing\n", req.domain_obj); return 0xE401; }
        }
        ipc_input_services(&req, sess, dom);
        target->handler(target, &req, &resp);
    } else if (req.type == 5 || req.type == 7) {  // control
        req.cmd = *(uint32_t *)(payload + 8);
        LOG("[ipc] control %u on %s (handle %x)\n", req.cmd, sess->svc->name, handle);
        switch (req.cmd) {
        case 0:  // ConvertCurrentObjectToDomain
            sess->domain = calloc(1, sizeof(Domain));
            sess->domain->objs[1] = sess->svc;
            resp_size(&resp, 4);
            *(uint32_t *)resp.data = 1;
            break;
        case 2: case 4: {  // CloneCurrentObject(Ex)
            uint32_t h = session_new(sess->svc);
            ((KSession *)handle_get(h))->domain = sess->domain;
            resp.move_handles[resp.nmove++] = h;
            break;
        }
        case 3:  // QueryPointerBufferSize
            *(uint16_t *)resp.data = 0x8000;
            resp_size(&resp, 4);
            break;
        default:
            LOG("[ipc] control cmd %u\n", req.cmd);
        }
    } else {
        LOG("[ipc] message type %u\n", req.type);
    }

write_response:;
    // returned objects: domain objects or new sessions
    uint32_t domain_out[8];
    int ndomain_out = 0;
    for (int i = 0; i < resp.nobjects; i++) {
        if (dom) {
            int id = 1;
            while (id < 256 && sess->domain->objs[id]) id++;
            sess->domain->objs[id] = resp.objects[i];
            resp.objects[i]->refs++;
            domain_out[ndomain_out++] = id;
        } else {
            resp.move_handles[resp.nmove++] = session_new(resp.objects[i]);
        }
    }
    memset(msg, 0, 0x100);
    uint32_t *w = msg;
    int has_handles = resp.ncopy || resp.nmove;
    uint32_t payload_words = 4 + (resp.data_size + 3) / 4;   // SFCO header + data
    if (dom) payload_words += 4 + ndomain_out;
    uint32_t raw_size = payload_words + 4;  // + alignment padding
    w[0] = 0;  // type 0 response
    w[1] = raw_size | (has_handles ? 0x80000000u : 0);
    uint32_t *q = w + 2;
    if (has_handles) {
        *q++ = (resp.ncopy << 1) | (resp.nmove << 5);
        for (int i = 0; i < resp.ncopy; i++) *q++ = resp.copy_handles[i];
        for (int i = 0; i < resp.nmove; i++) *q++ = resp.move_handles[i];
    }
    uint32_t *out = (uint32_t *)(((uintptr_t)q + 15) & ~(uintptr_t)15);
    if (dom) {
        out[0] = ndomain_out;
        out += 4;
    }
    memcpy(out, "SFCO", 4);
    out[1] = 0;
    out[2] = resp.result;
    out[3] = 0;
    memcpy(out + 4, resp.data, resp.data_size);
    if (dom)
        for (int i = 0; i < ndomain_out; i++) out[4 + (resp.data_size + 3) / 4 + i] = domain_out[i];
    return 0;
}
