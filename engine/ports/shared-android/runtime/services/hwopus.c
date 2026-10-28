/* Adapted from Let's Go Recomp (MIT), copyright (c) 2026 raulzito12345rrs.
 * See native/runtime/LICENSE-LetsGo and LICENSE-LASRecomp. */
// hwopus (nn::codec::HardwareOpusDecoder): Opus packets -> interleaved s16 PCM, decoded on the host with FFmpeg.
// Let's Go's music and sound effects are Opus; with the logging stub GetWorkBufferSize returned 0 and the game gave up
// on every decoder (its 6-channel audio out played silence).
//
// IHardwareOpusDecoderManager: 0 Open(params {u32 rate, u32 channels}, u32 work size, transfer memory),
//   1 GetWorkBufferSize(params), 2 OpenForMultiStream, 3 GetWorkBufferSizeForMultiStream (multistream: not seen yet).
// IHardwareOpusDecoder: 0 DecodeInterleavedOld, 4 DecodeInterleavedWithPerfOld, 6 DecodeInterleaved(u8 reset).
//   Input buffer: {u32 size BE, u32 final range BE} + packet. Output: s16 interleaved at the opened rate.
//   Returns u32 bytes consumed, u32 samples per channel (+ u64 time in microseconds for 4 / 6).
#include <libavcodec/avcodec.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

typedef struct {
    uint32_t rate, channels;
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    pthread_mutex_t lock;
} OpusDec;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static bool opus_open(OpusDec *d) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    // (the game aborts without it, right after the controller screen: say why instead - an FFmpeg built without
    // swresample has no Opus decoder, tools/build_deps_windows.sh)
    if (!codec) FATAL("the game's audio needs FFmpeg's Opus decoder, but this runtime was built without it\n");
    d->ctx = avcodec_alloc_context3(codec);
    if (!d->ctx) return false;
    d->ctx->sample_rate = 48000;  // (FFmpeg decodes Opus at 48 kHz: other rates are decimated below)
    av_channel_layout_default(&d->ctx->ch_layout, (int)d->channels);
    if (avcodec_open2(d->ctx, codec, NULL) < 0) { LOG("[hwopus] cannot open the Opus decoder\n"); return false; }
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    return d->pkt && d->frame;
}

static void opus_free(OpusDec *d) {
    if (!d) return;
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

static int16_t to_s16(float s) {
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    return (int16_t)(s * 32767.0f);
}

// One packet -> out (interleaved s16 at d->rate). Returns samples per channel, or -1.
static int opus_decode(OpusDec *d, const uint8_t *data, uint32_t size, int16_t *out, uint64_t out_cap) {
    av_packet_unref(d->pkt);
    d->pkt->data = (uint8_t *)data;
    d->pkt->size = (int)size;
    if (avcodec_send_packet(d->ctx, d->pkt) < 0) return -1;
    uint32_t step = d->rate && d->rate < 48000 ? 48000 / d->rate : 1;  // Opus rates divide 48 kHz exactly
    int total = 0;
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        AVFrame *f = d->frame;
        int ch = f->ch_layout.nb_channels;
        bool planar = f->format == AV_SAMPLE_FMT_FLTP;
        for (int i = 0; i + (int)step <= f->nb_samples; i += (int)step) {
            if ((uint64_t)(total + 1) * d->channels * 2 > out_cap) break;
            for (uint32_t c = 0; c < d->channels; c++) {
                int sc = (int)c < ch ? (int)c : ch - 1;
                float acc = 0;
                for (uint32_t k = 0; k < step; k++)
                    acc += planar ? ((const float *)f->data[sc])[i + k] : ((const float *)f->data[0])[(i + k) * ch + sc];
                out[total * d->channels + c] = to_s16(acc / (float)step);
            }
            total++;
        }
    }
    return total;
}

static void opus_decoder(Service *s, IpcReq *req, IpcResp *resp) {
    OpusDec *d = s->state;
    switch (req->cmd) {
    case 0: case 4: case 6: {  // DecodeInterleaved*(in packet buffer, out PCM buffer)
        IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
        if (!in || !out || in->size < 8) { resp->result = 111 | (6 << 9); return; }  // (module 111 codec: bad input)
        const uint8_t *p = (const uint8_t *)(uintptr_t)in->addr;
        uint32_t size = be32(p);
        if (8 + (uint64_t)size > in->size) { resp->result = 111 | (6 << 9); return; }
        if (req->cmd == 6 && req->data_size >= 1 && IN(uint8_t, 0)) {
            pthread_mutex_lock(&d->lock);
            avcodec_flush_buffers(d->ctx);  // reset: a new stream
            pthread_mutex_unlock(&d->lock);
        }
        uint64_t t0 = plat_now_ns();
        pthread_mutex_lock(&d->lock);
        int n = opus_decode(d, p + 8, size, (int16_t *)(uintptr_t)out->addr, out->size);
        pthread_mutex_unlock(&d->lock);
        if (n < 0) {
            static int nlog;
            if (nlog++ < 10) LOG("[hwopus] packet of %u bytes failed to decode\n", size);
            n = 0;
        }
        OUT(uint32_t, 0) = 8 + size;
        OUT(uint32_t, 4) = (uint32_t)n;
        if (req->cmd != 0) { OUT(uint64_t, 8) = (plat_now_ns() - t0) / 1000; resp_size(resp, 16); }
        else resp_size(resp, 8);
        break;
    }
    case 1: break;  // SetContext
    default:
        LOG("[hwopus] IHardwareOpusDecoder cmd %u unsupported\n", req->cmd);
        resp->result = 111 | (1 << 9);
        break;
    }
}

static void opus_decoder_close(Service *s) {
    OpusDec *d = s->state;
    s->state = NULL;
    opus_free(d);
}

// What a decoder of this many channels needs on the console (any non-zero size works: the work buffer is unused).
static uint32_t work_size(uint32_t channels) { return 0x4000 + channels * 0x8000; }

SERVICE("hwopus", hwopus) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // OpenHardwareOpusDecoder(params, work size, transfer memory)
        if (req->data_size < 8) { resp->result = 111 | (1 << 9); return; }
        OpusDec *d = calloc(1, sizeof *d);
        if (!d) { resp->result = 111 | (1 << 9); return; }
        d->rate = IN(uint32_t, 0);
        d->channels = IN(uint32_t, 4);
        pthread_mutex_init(&d->lock, NULL);
        bool valid_rate = d->rate == 8000 || d->rate == 12000 || d->rate == 16000 || d->rate == 24000 || d->rate == 48000;
        if (d->channels < 1 || d->channels > 2 || !valid_rate || !opus_open(d)) {
            LOG("[hwopus] cannot open a decoder for %u Hz, %u ch\n", d->rate, d->channels);
            opus_free(d);
            resp->result = 111 | (1 << 9);
            return;
        }
        static int nlog;
        if (nlog++ < 8) LOG("[hwopus] decoder opened: %u Hz, %u ch\n", d->rate, d->channels);
        Service *dec = service_new("IHardwareOpusDecoder", opus_decoder, d);
        dec->on_close = opus_decoder_close;
        ipc_return_object(resp, dec);
        break;
    }
    case 1:  // GetWorkBufferSize(params)
        if (req->data_size < 8) { resp->result = 111 | (1 << 9); return; }
        OUT(uint32_t, 0) = work_size(IN(uint32_t, 4));
        resp_size(resp, 4);
        break;
    default:
        LOG("[hwopus] IHardwareOpusDecoderManager cmd %u unsupported (multistream is not implemented)\n", req->cmd);
        resp->result = 111 | (1 << 9);
        break;
    }
}
