/* Shared Android service overlay, derived from pinned LAS Recomp Android (MIT).
 * Copyright (c) 2026 Peiikai. See native/runtime/LICENSE-LASRecomp.
 * Required generic deltas adapted from Let's Go Recomp (MIT), copyright (c) 2026 raulzito12345rrs;
 * see native/runtime/LICENSE-LetsGo.
 * REV9+ effect status layout adapted from EoWRecomp dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505,
 * copyright (c) 2026 Peiikai and EoWRecomp contributors (MIT); see native/runtime/LICENSE-Echoes. */
// audren:u (audio renderer) and audout:u.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>

#include <SDL3/SDL.h>

#include "../hle.h"
#include "../settings.h"
#include "../ipc.h"
#include "title_adapter.h"
#include "audren_work_size.h"

// ------------------------------------------------------------------ audren: a CPU mixer modelled on
// Ryujinx's AudioRenderer (VoiceState / DataSourceHelper / StateUpdater; MIT, Ryujinx Team and Contributors,
// third_party/ryujinx/LICENSE.txt).
// Voices decode PCM16 / ADPCM wave buffers, resample to 48 kHz, and mix into their destination mix's
// buffers; sub mixes route into their destination; the final mix feeds the device sink, which we
// downmix to stereo and push to an SDL audio stream. Voice/mix splitters and reverb are supported;
// other effects remain unsupported.
#define WB_COUNT 4
#define MIXBUF_MAX 24
#define HIST 4
#define AUDREN_MIX_MAX 64u
#define AUDREN_SPLITTER_MAX 256u
#define AUDREN_SPLITTER_DEST_MAX 4096u
#define AUDREN_UNUSED_MIX INT32_MAX

typedef struct {
    uint64_t addr, size, ctx;
    uint32_t start, end, loop_start, loop_end;
    int32_t loop_count;
    uint8_t loop, eos, sent;  // sent = IsSendToAudioProcessor
} WaveBuf;

typedef struct {  // per channel resource, "VoiceUpdateState"
    uint8_t valid[WB_COUNT];
    uint32_t wb_index, consumed;
    uint64_t played;
    int32_t offset, loop_count;
    float fraction;
    int16_t hist[HIST];
    int16_t pred_scale, h0, h1;
    float bq[2][2];  // biquad filter state per voice filter
} ChanState;

typedef struct {
    float mix[MIXBUF_MAX], prev[MIXBUF_MAX];
    uint8_t used;
} ChanRes;

typedef struct {
    uint8_t in_use, is_new, play_state;  // play_state: 0 started, 1 stopped, 2 stopping, 3 paused
    uint8_t fmt, chans, drop;
    uint32_t rate, wb_count, flush;
    float pitch, volume, prev_volume;
    int32_t mix_id;
    uint32_t splitter_id;
    uint32_t chan_ids[6];
    uint64_t ds_addr;
    uint16_t behaviour;
    uint8_t bq_on[2], bq_reset[2];
    float bq[2][5];  // a0 a1 a2 b1 b2
    WaveBuf wb[WB_COUNT];
} Voice;

static atomic_uint audren_decode_warning_count;

static void warn_unsupported_voice_decode(const Voice *v, int channel, const char *reason) {
    if (!getenv("LARECOMP_AUDIO_LOG")) return;
    unsigned count = atomic_load_explicit(&audren_decode_warning_count, memory_order_relaxed);
    while (count < 8) {
        if (atomic_compare_exchange_weak_explicit(&audren_decode_warning_count, &count, count + 1,
                                                  memory_order_relaxed, memory_order_relaxed)) {
            LOG("[audren] voice decode unavailable: %s fmt %u chans %u rate %u channel %d\n",
                reason, v->fmt, v->chans, v->rate, channel);
            return;
        }
    }
}

typedef struct {
    uint8_t used;
    float volume;
    uint32_t bufs;
    int32_t dest;
    uint32_t dest_splitter;
    float vol[MIXBUF_MAX * MIXBUF_MAX];
    float *buf;  // bufs * sample_count
} Mix;

// Independent implementation of the SNDH/SNDI/SNDD layouts and mix routing
// described by yuzu/Eden audio_core/renderer/splitter and GenerateMixCommands
// (2022 yuzu, GPL-2.0-or-later; 2025/2026 Eden, GPL-3.0-or-later).
// No reference implementation text is copied.
typedef struct { uint32_t offset, count; } MixSplitterInfo;
typedef struct {
    int32_t mix;
    uint8_t used, touched;
    float volume[MIXBUF_MAX], previous[MIXBUF_MAX];
} MixSplitterDest;
typedef struct {
    MixSplitterInfo *info;
    MixSplitterDest *dest;
    uint32_t *ids;
} MixSplitters;

typedef struct Effect Effect;

typedef struct {
    uint32_t sample_rate, sample_count, mix_buffers, submixes, voices, sinks, effects, perf_frames;
    uint32_t splitters, splitter_destinations, revision;
    MixSplitters *routing;
    uint8_t mix_order[AUDREN_MIX_MAX];
    uint32_t mix_order_count;
    uint64_t elapsed_frames;
    uint32_t event_r, event_w;
    KEvent *event;
    uint32_t state;  // 0 started, 1 stopped
    pthread_mutex_t lock;
    Voice *voice;
    ChanRes *chres;
    ChanState *chst;
    Mix *mix;
    uint32_t nmix;
    uint8_t sink_used, sink_inputs, sink_input[6];
    Effect *fx;
    SDL_AudioStream *stream;
    FILE *wav;
    uint32_t wav_bytes;
    pthread_t thread;
    volatile bool quit;  // reboot (audio_reset)
} Renderer;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// One sample of DSP ADPCM at absolute sample index s (frames of 8 bytes: header + 14 nibbles).
static int16_t adpcm_next(const uint8_t *data, uint64_t size, uint32_t s, const int16_t *coefs, ChanState *cs) {
    uint32_t frame = s / 14, n = s % 14;
    uint64_t hdr = (uint64_t)frame * 8;
    if (hdr + 1 + n / 2 >= size) return 0;
    uint8_t ps = data[hdr];
    int scale = ps & 15, ci = (ps >> 4) & 7;
    uint8_t b = data[hdr + 1 + n / 2];
    int nib = (n & 1) ? (int8_t)(b << 4) >> 4 : (int8_t)b >> 4;
    int pred = coefs[ci * 2] * cs->h0 + coefs[ci * 2 + 1] * cs->h1;
    int v = ((nib * (2048 << scale)) + pred + 1024) >> 11;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    cs->h1 = cs->h0;
    cs->h0 = (int16_t)v;
    return (int16_t)v;
}

static void mark_end(ChanState *cs, WaveBuf *wb, uint32_t *idx) {
    cs->valid[*idx] = 0;
    *idx = (*idx + 1) % WB_COUNT;
    cs->loop_count = 0;
    cs->consumed++;
    if (wb->eos) cs->played = 0;
}

// Decode up to `need` samples of channel `ch` into out; returns how many were produced.
static int decode_voice(Voice *v, int ch, ChanState *cs, int16_t *out, int need) {
    int y = 0;
    uint32_t idx = cs->wb_index;
    const int16_t *coefs = (const int16_t *)(uintptr_t)v->ds_addr;
    while (y < need) {
        if (!cs->valid[idx]) break;  // starving
        WaveBuf *wb = &v->wb[idx];
        if (cs->offset == 0 && v->fmt == 6 && wb->ctx) {
            const int16_t *c = (const int16_t *)(uintptr_t)wb->ctx;
            cs->pred_scale = c[0]; cs->h0 = c[1]; cs->h1 = c[2];
        }
        uint32_t start = wb->start, end = wb->end;
        if (cs->loop_count > 0 && wb->loop_start && wb->loop_end && wb->loop_start <= wb->loop_end) {
            start = wb->loop_start;
            end = wb->loop_end;
        }
        int total = (int)end - (int)start, n = 0;
        int avail = total - cs->offset;
        if (avail < 0) avail = 0;
        int want = need - y < avail ? need - y : avail;
        const uint8_t *data = (const uint8_t *)(uintptr_t)wb->addr;
        if (data && wb->size) {
            if (v->fmt == 2) {  // PCM16 interleaved
                const int16_t *pcm = (const int16_t *)data;
                uint64_t nsamp = wb->size / 2 / v->chans;
                for (; n < want; n++) {
                    uint64_t s = (uint64_t)start + cs->offset + n;
                    if (s >= nsamp) break;
                    out[y + n] = pcm[s * v->chans + ch];
                }
            } else if (v->fmt == 6 && coefs) {
                for (; n < want; n++) out[y + n] = adpcm_next(data, wb->size, start + cs->offset + n, coefs, cs);
            } else if (v->fmt == 3) {  // PCM float
                const float *pcm = (const float *)data;
                uint64_t nsamp = wb->size / 4 / v->chans;
                for (; n < want; n++) {
                    uint64_t s = (uint64_t)start + cs->offset + n;
                    if (s >= nsamp) break;
                    out[y + n] = (int16_t)clampf(pcm[s * v->chans + ch] * 32767.f, -32768.f, 32767.f);
                }
            } else if (want > 0 && v->fmt == 6) {
                warn_unsupported_voice_decode(v, ch, "missing ADPCM coefficients");
            } else if (want > 0) {
                warn_unsupported_voice_decode(v, ch, "unsupported format");
            }
        }
        y += n;
        cs->offset += n;
        cs->played += n;
        if (cs->offset >= total || n == 0) {
            cs->offset = 0;
            if (wb->loop) {
                cs->loop_count++;
                if (wb->loop_count >= 0 && (n == 0 || cs->loop_count > wb->loop_count)) mark_end(cs, wb, &idx);
                if (n == 0) break;
                if (v->behaviour & 1) cs->played = 0;  // PlayedSampleCountResetWhenLooping
            } else {
                mark_end(cs, wb, &idx);
            }
        }
    }
    cs->wb_index = idx;
    return y;
}

// UpdateParametersForCommandGeneration: returns whether the voice should be played this frame.
static int voice_prepare(Renderer *r, Voice *v) {
    int nch = v->chans > 6 ? 6 : v->chans;
    if (v->is_new) {
        for (int c = 0; c < nch; c++) memset(&r->chst[v->chan_ids[c] % r->voices], 0, sizeof(ChanState));
        v->prev_volume = v->volume;
        v->is_new = 0;
    }
    if (v->flush) {
        uint32_t wi = 0;
        for (uint32_t i = 0; i < v->flush; i++) {
            for (int c = 0; c < nch; c++) {
                ChanState *cs = &r->chst[v->chan_ids[c] % r->voices];
                if (i == 0) wi = cs->wb_index;
                cs->valid[(wi + i) % WB_COUNT] = 0;
                cs->wb_index = (cs->wb_index + 1) % WB_COUNT;
                cs->consumed++;
            }
            v->wb[(wi + i) % WB_COUNT].sent = 1;
        }
        v->flush = 0;
    }
    if (v->play_state == 0) {
        for (int i = 0; i < WB_COUNT; i++) {
            if (v->wb[i].sent) continue;
            for (int c = 0; c < nch; c++) r->chst[v->chan_ids[c] % r->voices].valid[i] = 1;
            v->wb[i].sent = 1;
        }
        ChanState *p = &r->chst[v->chan_ids[0] % r->voices];
        for (int i = 0; i < WB_COUNT; i++) if (p->valid[i]) return 1;
        return 0;
    }
    if (v->play_state == 2) {  // stopping: drop everything queued
        for (int i = 0; i < WB_COUNT; i++) {
            v->wb[i].sent = 1;
            for (int c = 0; c < nch; c++) {
                ChanState *cs = &r->chst[v->chan_ids[c] % r->voices];
                if (cs->valid[i]) { cs->wb_index = (cs->wb_index + 1) % WB_COUNT; cs->consumed++; }
                cs->valid[i] = 0;
            }
        }
        for (int c = 0; c < nch; c++) {
            ChanState *cs = &r->chst[v->chan_ids[c] % r->voices];
            cs->offset = 0; cs->played = 0; cs->fraction = 0; cs->loop_count = 0;
            memset(cs->hist, 0, sizeof cs->hist);
            cs->pred_scale = cs->h0 = cs->h1 = 0;
        }
        v->play_state = 1;
    }
    return 0;
}

static Mix *find_mix(Renderer *r, int32_t id) { return id >= 0 && (uint32_t)id < r->nmix && r->mix[id].used ? &r->mix[id] : NULL; }

// ------------------------------------------------------------------ reverb (port of Ryujinx ReverbState/Command)
typedef struct { float *buf; uint32_t sr, cur, last, count, max; } DelayLine;

static void dl_set(DelayLine *d, float ms) {
    uint32_t n = (uint32_t)roundf(d->sr * ms);
    d->count = n < d->max ? n : d->max;
    d->cur = 0;
    d->last = d->count ? d->count - 1 : 0;
}
static void dl_init(DelayLine *d, uint32_t sr, float ms_max) {
    d->sr = sr;
    d->max = (uint32_t)roundf(sr * ms_max);
    free(d->buf);
    d->buf = calloc(d->max + 1, sizeof(float));
    dl_set(d, ms_max);
}
static float dl_read(DelayLine *d) { return d->buf[d->cur]; }
static float dl_update(DelayLine *d, float v) {
    float o = d->buf[d->cur];
    d->buf[d->cur++] = v;
    if (d->cur >= d->last) d->cur = 0;
    return o;
}
static float dl_tap_unsafe(DelayLine *d, uint32_t idx, int off) {
    int t = (int)d->cur - ((int)idx + off);
    if (t < 0) t += (int)d->count;
    if (t < 0 || (uint32_t)t > d->max) return 0;
    return d->buf[t];
}
static float dl_tap(DelayLine *d, uint32_t idx) {
    if (idx >= d->count) idx = d->count ? d->count - 1 : 0;
    return dl_tap_unsafe(d, idx, -1);
}

typedef struct { DelayLine dl; float rate; } DecayDelay;
static float dd_update(DecayDelay *d, float v) {
    float p = v - d->rate * dl_read(&d->dl);
    return dl_update(&d->dl, p) + p * d->rate;
}

typedef struct {
    uint8_t input[6], output[6];
    uint16_t ch_max, ch;
    int32_t sample_rate, early_mode, early_gain, pre_delay, late_mode, late_gain, decay_time, hf_decay_ratio,
        coloration, reverb_gain, out_gain, dry_gain;
} ReverbParam;

typedef struct {
    int ready;
    uint8_t raw[0x44];  // last parameter bytes seen
    ReverbParam p;
    DelayLine fdn[4], pre, fc;
    DecayDelay decay[4];
    uint32_t early_time[10], pre_time;
    float early_gain[10], hf_direct[4], hf_prev[4], prev_fb[4];
} Reverb;

static const float rv_fdn_times[20] = {53.953247f, 79.192566f, 116.238770f, 130.615295f, 53.953247f, 79.192566f,
    116.238770f, 170.615295f, 5, 10, 5, 10, 47.03f, 71, 103, 170, 53.953247f, 79.192566f, 116.238770f, 170.615295f};
static const float rv_decay_times[20] = {7, 9, 13, 17, 7, 9, 13, 17, 1, 1, 1, 1, 7, 7, 13, 9, 7, 9, 13, 17};
static const float rv_early_times[50] = {0.0f, 3.5f, 2.8f, 3.9f, 2.7f, 13.4f, 7.9f, 8.4f, 9.9f, 12.0f,
    0.0f, 11.8f, 5.5f, 11.2f, 10.4f, 38.1f, 22.2f, 29.6f, 21.2f, 24.8f,
    0.0f, 41.5f, 20.5f, 41.3f, 0.0f, 29.5f, 33.8f, 45.2f, 46.8f, 0.0f,
    33.1f, 43.3f, 22.8f, 37.9f, 14.9f, 35.3f, 17.9f, 34.2f, 0.0f, 43.3f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const float rv_early_gain[50] = {0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.68f, 0.68f,
    0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.68f, 0.68f, 0.68f, 0.68f,
    0.50f, 0.70f, 0.70f, 0.68f, 0.50f, 0.68f, 0.68f, 0.70f, 0.68f, 0.00f,
    0.93f, 0.92f, 0.87f, 0.86f, 0.94f, 0.81f, 0.80f, 0.77f, 0.76f, 0.65f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const float rv_pre_times[5] = {12.5f, 40.0f, 50.0f, 50.0f, 0.0f};

static float q14(int32_t v) { return v / 16384.0f; }
static uint32_t rv_rate(const ReverbParam *p) {
    uint32_t sr = (uint32_t)q14(p->sample_rate);  // samples per millisecond
    return sr && sr <= 192 ? sr : 48;
}

static void reverb_update(Reverb *rv) {
    ReverbParam *p = &rv->p;
    uint32_t sr = rv_rate(p);
    float pre_ms = q14(p->pre_delay), early_gain = q14(p->early_gain), coloration = q14(p->coloration);
    float decay_time = q14(p->decay_time);
    int em = p->early_mode >= 0 && p->early_mode < 5 ? p->early_mode : 4;
    int lm = p->late_mode >= 0 && p->late_mode < 5 ? p->late_mode : 0;
    for (int i = 0; i < 10; i++) {
        uint32_t t = (uint32_t)roundf(sr * (rv_early_times[i] + pre_ms));
        rv->early_time[i] = (t < rv->pre.max ? t : rv->pre.max) + 1;
        rv->early_gain[i] = rv_early_gain[i] * early_gain;
    }
    if (p->ch == 2) { rv->early_gain[4] *= 0.5f; rv->early_gain[5] *= 0.5f; }
    uint32_t pt = (uint32_t)roundf(sr * (rv_pre_times[em] + pre_ms));
    rv->pre_time = pt < rv->pre.max ? pt : rv->pre.max;
    float hf_ratio = q14(p->hf_decay_ratio), hf_cos = cosf(1280.0f / sr);
    for (int i = 0; i < 4; i++) {
        dl_set(&rv->fdn[i], rv_fdn_times[lm * 4 + i]);
        dl_set(&rv->decay[i].dl, rv_decay_times[lm * 4 + i]);
        float a = -3.0f * (rv->decay[i].dl.count + rv->fdn[i].count);
        float b = decay_time > 0 ? a / (decay_time * sr) : -1000.0f;
        float c, d;
        if (hf_ratio < 0.995f && hf_ratio > 0) {
            float e = powf(10.0f, (((1.0f / hf_ratio) - 1.0f) * 2) / 100 * (b / 10));
            float f = 1.0f - e, g = 2.0f - (e * 2 * hf_cos);
            float h = sqrtf(fmaxf(0, g * g - f * f * 4));
            c = f != 0 ? (g - h) / (f * 2) : 0;
            d = 1.0f - c;
        } else {
            c = 0;
            d = 1;
        }
        rv->hf_direct[i] = powf(10.0f, b / 1000) * d * 0.7071f;
        rv->hf_prev[i] = c;
        rv->prev_fb[i] = 0;
        rv->decay[i].rate = 0.6f * (1.0f - coloration);
    }
}

static void reverb_init(Reverb *rv) {
    uint32_t sr = rv_rate(&rv->p);
    for (int i = 0; i < 4; i++) {
        dl_init(&rv->fdn[i], sr, rv_fdn_times[16 + i]);
        dl_init(&rv->decay[i].dl, sr, rv_decay_times[16 + i]);
    }
    dl_init(&rv->pre, sr, 150.0f);  // long pre-delay needs REV3+... 350 ms
    dl_init(&rv->fc, sr, 5.0f);
    reverb_update(rv);
    rv->ready = 1;
}

static void reverb_process(Reverb *rv, float *const *in, float *const *out, uint32_t n) {
    static const int8_t oe1[10] = {0}, te[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static const int8_t o1[4] = {0}, tf1[4] = {0, 1, 2, 3};
    static const int8_t oe2[10] = {0, 0, 1, 1, 0, 1, 0, 0, 1, 1}, o2[4] = {0, 0, 1, 1}, tf2[4] = {2, 0, 3, 1};
    static const int8_t oe4[10] = {0, 0, 1, 1, 0, 1, 2, 2, 3, 3}, o4[4] = {0, 1, 2, 3}, tf4[4] = {0, 1, 2, 3};
    static const int8_t oe6[20] = {0, 5, 0, 5, 1, 5, 1, 5, 4, 5, 4, 5, 2, 5, 2, 5, 3, 5, 3, 5};
    static const int8_t te6[20] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9};
    static const int8_t o6[6] = {0, 1, 2, 3, 4, 5}, tf6[6] = {0, 1, 2, 3, -1, 3};
    int ch = rv->p.ch;
    const int8_t *oe, *tel, *o, *tf;
    int ne, nf;
    switch (ch) {
    case 1: oe = oe1; tel = te; o = o1; tf = tf1; ne = 10; nf = 4; break;
    case 2: oe = oe2; tel = te; o = o2; tf = tf2; ne = 10; nf = 4; break;
    case 4: oe = oe4; tel = te; o = o4; tf = tf4; ne = 10; nf = 4; break;
    case 6: oe = oe6; tel = te6; o = o6; tf = tf6; ne = 20; nf = 6; break;
    default: return;
    }
    float reverb_gain = q14(rv->p.reverb_gain), late_gain = q14(rv->p.late_gain);
    float out_gain = q14(rv->p.out_gain), dry_gain = q14(rv->p.dry_gain);
    for (uint32_t s = 0; s < n; s++) {
        float ov[6] = {0}, fb[4], fo[4], cin[6];
        for (int i = 0; i < ne; i++) ov[oe[i]] += dl_tap_unsafe(&rv->pre, rv->early_time[tel[i]], 0) * rv->early_gain[tel[i]];
        if (ch == 6) ov[5] *= 0.2f;
        float pre = 0;
        for (int c = 0; c < ch; c++) { cin[c] = in[c][s] * 64; pre += cin[c] * reverb_gain; }
        dl_update(&rv->pre, pre);
        float late = dl_tap(&rv->pre, rv->pre_time) * late_gain;
        for (int i = 0; i < 4; i++) {
            fo[i] = dl_read(&rv->fdn[i]) * rv->hf_direct[i] + rv->prev_fb[i] * rv->hf_prev[i];
            rv->prev_fb[i] = fo[i];
        }
        fb[0] = fo[2] + fo[1];
        fb[1] = -fo[0] - fo[3];
        fb[2] = fo[0] - fo[3];
        fb[3] = fo[1] - fo[2];
        for (int i = 0; i < 4; i++) {
            fo[i] = dd_update(&rv->decay[i], fb[i] + late);
            dl_update(&rv->fdn[i], fo[i]);
        }
        for (int i = 0; i < nf; i++) if (tf[i] >= 0) ov[o[i]] += fo[tf[i]];
        if (ch == 6) ov[4] += dl_update(&rv->fc, (fo[2] - fo[3]) * 0.5f);
        for (int c = 0; c < ch; c++) out[c][s] = (ov[c] * out_gain + cin[c] * dry_gain) / 64;
    }
}

struct Effect {
    uint8_t type, enabled;
    int32_t mix_id;
    uint32_t order;
    Reverb rv;
};

// Effect slot update from EffectInParameterVersion1 (0xC0 bytes: type, is_new, enabled, mix, buffers, order, data).
static void effect_update(Effect *fx, const uint8_t *e) {
    if (fx->type != e[0]) memset(&fx->rv.p, 0, sizeof fx->rv.p), fx->rv.ready = 0;
    fx->type = e[0];
    fx->enabled = e[2];
    memcpy(&fx->mix_id, e + 4, 4);
    memcpy(&fx->order, e + 0x18, 4);
    if (fx->type != 4) return;
    const uint8_t *d = e + 0x20;
    uint16_t ch_max, ch;
    memcpy(&ch_max, d + 12, 2);
    memcpy(&ch, d + 14, 2);
    if (ch_max != 1 && ch_max != 2 && ch_max != 4 && ch_max != 6) return;
    if (ch != 1 && ch != 2 && ch != 4 && ch != 6) return;
    if (e[1] || !fx->rv.ready) {
        memcpy(fx->rv.raw, d, sizeof fx->rv.raw);
        memcpy(&fx->rv.p, d, sizeof fx->rv.p);
        if (getenv("LARECOMP_AUDIO_LOG"))
            LOG("[audren] reverb init: ch %u rate %d early %d/%d pre %d late %d/%d decay %d hf %d col %d gains %d/%d/%d\n",
                ch, fx->rv.p.sample_rate, fx->rv.p.early_mode, fx->rv.p.early_gain, fx->rv.p.pre_delay,
                fx->rv.p.late_mode, fx->rv.p.late_gain, fx->rv.p.decay_time, fx->rv.p.hf_decay_ratio,
                fx->rv.p.coloration, fx->rv.p.reverb_gain, fx->rv.p.out_gain, fx->rv.p.dry_gain);
        reverb_init(&fx->rv);
    } else if (memcmp(fx->rv.raw, d, 0x40)) {  // parameters changed (status byte at 0x40 ignored)
        memcpy(fx->rv.raw, d, sizeof fx->rv.raw);
        memcpy(&fx->rv.p, d, sizeof fx->rv.p);
        reverb_update(&fx->rv);
    }
}

static void apply_effects(Renderer *r, int32_t mix_id, Mix *m) {
    uint32_t N = r->sample_count;
    for (uint32_t order = 0; order < r->effects; order++) {
        for (uint32_t i = 0; i < r->effects; i++) {
            Effect *fx = &r->fx[i];
            if (fx->type != 4 || fx->mix_id != mix_id || fx->order != order || !fx->rv.ready) continue;
            int ch = fx->rv.p.ch;
            float *in[6], *out[6];
            int ok = 1;
            for (int c = 0; c < ch; c++) {
                if (fx->rv.p.input[c] >= m->bufs || fx->rv.p.output[c] >= m->bufs) ok = 0;
                in[c] = m->buf + (fx->rv.p.input[c] % MIXBUF_MAX) * N;
                out[c] = m->buf + (fx->rv.p.output[c] % MIXBUF_MAX) * N;
            }
            if (!ok) continue;
            static int no_reverb = -1;
            if (no_reverb < 0) no_reverb = getenv("LARECOMP_NO_REVERB") ? 1 : 0;
            static int toggle = -1;
            if (toggle < 0) toggle = getenv("LARECOMP_REVERB_TOGGLE") ? atoi(getenv("LARECOMP_REVERB_TOGGLE")) : 0;
            int off = no_reverb || !g_settings.reverb || (toggle && (r->elapsed_frames / toggle) % 2);
            if (fx->enabled && !off) reverb_process(&fx->rv, in, out, N);
            else
                for (int c = 0; c < ch; c++) if (in[c] != out[c]) memcpy(out[c], in[c], N * sizeof(float));
        }
    }
}

static bool audio_route_log_slot(atomic_uint *counter);

static int hf_log = -1;
static double hf[1024];

static void render_frame(Renderer *r, float *stereo) {
    if (hf_log < 0) hf_log = getenv("LARECOMP_AUDIO_HF") ? 1 : 0;
    uint32_t N = r->sample_count;
    for (uint32_t m = 0; m < r->nmix; m++)
        if (r->mix[m].buf) memset(r->mix[m].buf, 0, sizeof(float) * MIXBUF_MAX * N);
    int16_t tmp[HIST + 4096];
    float smp[4096];
    for (uint32_t vi = 0; vi < r->voices; vi++) {
        Voice *v = &r->voice[vi];
        if (!v->in_use) continue;
        int play = voice_prepare(r, v);
        if (!play || v->wb_count == 0 || v->drop) continue;
        Mix *dst = find_mix(r, v->mix_id);
        float ratio = (float)v->rate / r->sample_rate * v->pitch;
        if (!(ratio > 0) || ratio > 8) ratio = 1;
        int nch = v->chans > 6 ? 6 : v->chans;
        for (int c = 0; c < nch; c++) {
            uint32_t rid = v->chan_ids[c] % r->voices;
            ChanState *cs = &r->chst[rid];
            ChanRes *cr = &r->chres[rid];
            int need = (int)(cs->fraction + ratio * N);
            if (need > 4096) need = 4096;
            memcpy(tmp, cs->hist, sizeof cs->hist);
            int got = decode_voice(v, c, cs, tmp + HIST, need);
            if (got < need) memset(tmp + HIST + got, 0, (need - got) * 2);
            float pos = cs->fraction;
            for (uint32_t i = 0; i < N; i++, pos += ratio) {
                int k = (int)pos;
                float f = pos - k;
                if (k + 3 > need + HIST - 1) k = need + HIST - 4;
                float y0 = tmp[k], y1 = tmp[k + 1], y2 = tmp[k + 2], y3 = tmp[k + 3];
                if (ratio == 1.0f && f == 0) { smp[i] = y1; continue; }
                // 4-point Hermite between y1 and y2
                float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3;
                float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                smp[i] = ((c3 * f + c2) * f + c1) * f + y1;
            }
            for (int fi = 0; fi < 2; fi++) {
                if (!v->bq_on[fi]) continue;
                float *st = cs->bq[fi], *k = v->bq[fi];
                if (v->bq_reset[fi]) st[0] = st[1] = 0;
                for (uint32_t i = 0; i < N; i++) {
                    float in = smp[i], o = in * k[0] + st[0];
                    st[0] = in * k[1] + o * k[3] + st[1];
                    st[1] = in * k[2] + o * k[4];
                    smp[i] = o;
                }
            }
            cs->fraction = cs->fraction + ratio * N - need;
            if (cs->fraction < 0 || cs->fraction >= 1) cs->fraction = 0;
            memcpy(cs->hist, tmp + need, sizeof cs->hist);
            if (hf_log) {
                // Goertzel power around 19.6 kHz (scaled by volume)
                double w = 2 * M_PI * 19582.0 / r->sample_rate, cw = 2 * cos(w), q1 = 0, q2 = 0;
                for (uint32_t i = 0; i < N; i++) { double q0 = cw * q1 - q2 + smp[i]; q2 = q1; q1 = q0; }
                double e = (q1 * q1 + q2 * q2 - q1 * q2 * cw) * v->volume * v->volume / N;
                hf[vi] += e;
            }
            static int solo = -2;
            if (solo == -2) solo = getenv("LARECOMP_AUDIO_SOLO") ? atoi(getenv("LARECOMP_AUDIO_SOLO")) : -1;
            if (solo >= 0 && (int)vi != solo) continue;
            if (v->mix_id == AUDREN_UNUSED_MIX && r->routing && v->splitter_id < r->splitters) {
                MixSplitterInfo *info = &r->routing->info[v->splitter_id];
                for (uint32_t ordinal = (uint32_t)c; ordinal < info->count; ordinal += (uint32_t)nch) {
                    uint32_t id = r->routing->ids[info->offset + ordinal];
                    MixSplitterDest *d = &r->routing->dest[id];
                    Mix *target = d->used ? find_mix(r, d->mix) : NULL;
                    if (!target || !target->buf) continue;
                    if (getenv("LARECOMP_AUDIO_LOG")) {
                        static atomic_uint reports;
                        if (audio_route_log_slot(&reports)) {
                            LOG("[audren] voice splitter route accepted: voice %u channel %d splitter %u ordinal %u destination %u mix %d buffers %u voice_volume %.6f->%.6f gains:",
                                vi, c, v->splitter_id, ordinal, id, d->mix, target->bufs, v->prev_volume, v->volume);
                            for (uint32_t b = 0; b < target->bufs; b++)
                                if (d->previous[b] || d->volume[b]) LOG(" %u=%.6f->%.6f", b, d->previous[b], d->volume[b]);
                            LOG("\n");
                        }
                    }
                    for (uint32_t b = 0; b < target->bufs; b++) {
                        float g0 = d->previous[b], dg = (d->volume[b] - g0) / N;
                        float dv = (v->volume - v->prev_volume) / N;
                        if (!g0 && !dg) continue;
                        float *o = target->buf + b * N;
                        for (uint32_t i = 0; i < N; i++)
                            o[i] += smp[i] * (v->prev_volume + dv * i) * (g0 + dg * i);
                    }
                    d->touched = 1;
                }
            } else if (dst) for (uint32_t b = 0; b < dst->bufs && b < MIXBUF_MAX; b++) {
                float g0 = v->prev_volume * cr->prev[b], g1 = v->volume * cr->mix[b];
                if (g0 == 0 && g1 == 0) continue;
                float *o = dst->buf + b * N, dg = (g1 - g0) / N;
                for (uint32_t i = 0; i < N; i++) o[i] += smp[i] * (g0 + dg * i);
            }
        }
        for (int c = 0; c < nch; c++) {
            ChanRes *cr = &r->chres[v->chan_ids[c] % r->voices];
            memcpy(cr->prev, cr->mix, sizeof cr->prev);
        }
        v->bq_reset[0] = v->bq_reset[1] = 0;
        v->prev_volume = v->volume;
    }
    // Voice destination ramps share one previous state for the whole frame.
    // Mix-splitter sends intentionally use current volumes without this ramp.
    if (r->routing) for (uint32_t i = 0; i < r->splitter_destinations; i++) {
        MixSplitterDest *d = &r->routing->dest[i];
        if (d->touched && d->used)
            for (uint32_t j = 0; j < MIXBUF_MAX; j++)
                if (isfinite(d->volume[j])) d->previous[j] = d->volume[j];
        d->touched = 0;
    }
    if (hf_log && r->elapsed_frames % 200 == 0) {
        LOG("[audren] hf f%llu:", (unsigned long long)r->elapsed_frames);
        for (uint32_t k = 0; k < r->voices; k++)
            if (hf[k] > 1e6)
                LOG(" v%u=%.0f(%llx bq %d%d %.3f/%.3f fmt %u rate %u pitch %.3f)", k, hf[k] / 1e6,
                    (unsigned long long)r->voice[k].wb[0].addr, r->voice[k].bq_on[0], r->voice[k].bq_on[1],
                    r->voice[k].bq[1][0], r->voice[k].bq[1][3], r->voice[k].fmt, r->voice[k].rate, r->voice[k].pitch);
        LOG("\n");
        memset(hf, 0, sizeof hf);
    }
    // All sources precede their destinations, including splitter fan-out joins.
    for (uint32_t index = 0; index < r->mix_order_count; index++) {
        uint32_t m = r->mix_order[index];
        Mix *s = &r->mix[m];
        apply_effects(r, (int32_t)m, s);
        if (s->dest == AUDREN_UNUSED_MIX) {
            if (!r->routing || s->dest_splitter >= r->splitters || !s->bufs) continue;
            MixSplitterInfo *info = &r->routing->info[s->dest_splitter];
            for (uint32_t i = 0; i < info->count; i++) {
                MixSplitterDest *dest = &r->routing->dest[r->routing->ids[info->offset + i]];
                Mix *t = dest->used ? find_mix(r, dest->mix) : NULL;
                if (!t || !t->buf) continue;
                float *src = s->buf + (i % s->bufs) * N;
                for (uint32_t j = 0; j < t->bufs; j++) {
                    float g = s->volume * dest->volume[j];
                    if (!g) continue;
                    float *o = t->buf + j * N;
                    for (uint32_t k = 0; k < N; k++) o[k] += src[k] * g;
                }
            }
        } else {
            Mix *t = find_mix(r, s->dest);
            if (!t || !t->buf) continue;
            for (uint32_t i = 0; i < s->bufs; i++)
                for (uint32_t j = 0; j < t->bufs; j++) {
                    float g = s->volume * s->vol[i * MIXBUF_MAX + j];
                    if (!g) continue;
                    float *src = s->buf + i * N, *o = t->buf + j * N;
                    for (uint32_t k = 0; k < N; k++) o[k] += src[k] * g;
                }
        }
    }
    // final mix -> device sink (FL FR FC LFE BL BR) -> stereo
    Mix *f = find_mix(r, 0);
    if (f) apply_effects(r, 0, f);
    for (uint32_t i = 0; i < N; i++) {
        float ch[6] = {0};
        if (f && r->sink_used)
            for (int c = 0; c < r->sink_inputs && c < 6; c++)
                if (r->sink_input[c] < f->bufs) ch[c] = f->buf[r->sink_input[c] * N + i] * f->volume;
        float L = ch[0], R = ch[1];
        if (r->sink_inputs >= 6) {
            L += 0.707f * ch[2] + 0.251f * ch[3] + 0.707f * ch[4];
            R += 0.707f * ch[2] + 0.251f * ch[3] + 0.707f * ch[5];
        }
        stereo[i * 2] = clampf(L / 32768.f, -1, 1);
        stereo[i * 2 + 1] = clampf(R / 32768.f, -1, 1);
    }
}

// Output gain: menu volume times LARECOMP_AUDIO_GAIN (applied live).
static void apply_gain(SDL_AudioStream *st) {
    static double env = -1;
    if (env < 0) env = getenv("LARECOMP_AUDIO_GAIN") ? atof(getenv("LARECOMP_AUDIO_GAIN")) : 1.0;
    float g = (float)(env * g_settings.volume);
    if (st && SDL_GetAudioStreamGain(st) != g) SDL_SetAudioStreamGain(st, g);
}

static void wav_header(FILE *f, uint32_t bytes) {
    uint32_t h[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020003, 48000, 48000 * 8,
                      0x00200008, 0x61746164, bytes};
    fseek(f, 0, SEEK_SET);
    fwrite(h, 4, 11, f);
    fseek(f, 0, SEEK_END);
}

static void *renderer_tick(void *arg) {
    plat_thread_name("audren");
    Renderer *r = arg;
    float out[2 * 4096];
    uint32_t N = r->sample_count > 4096 ? 4096 : r->sample_count;
    int frame_bytes = (int)(N * 2 * sizeof(float));
    uint64_t next = 0;
    struct timespec now;
    int target = 4 * frame_bytes;  // queued audio to keep ahead of the device: >= 2 device periods
    if (r->stream) {
        SDL_AudioSpec dev;
        int dev_frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(r->stream), &dev, &dev_frames) && dev_frames > 0) {
            int t = (2 * dev_frames * (int)r->sample_rate / (dev.freq ? dev.freq : 48000) + (int)N) * 2 * (int)sizeof(float);
            if (t > target) target = t;
            LOG("[audren] device %d Hz, %d frames/period; queue target %d samples\n", dev.freq, dev_frames,
                target / (2 * (int)sizeof(float)));
        }
    }
    while (!r->quit) {
        if (r->stream) {
            while (SDL_GetAudioStreamQueued(r->stream) >= target && !r->quit) {
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
            }
        } else {
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t t = now.tv_sec * 1000000000ull + now.tv_nsec;
            if (!next) next = t;
            if (next > t) { struct timespec ts = {0, (long)(next - t)}; nanosleep(&ts, NULL); }
            next += (uint64_t)N * 1000000000ull / r->sample_rate;
        }
        pthread_mutex_lock(&r->lock);
        if (r->state == 0) {
            render_frame(r, out);
            r->elapsed_frames++;
        } else {
            memset(out, 0, frame_bytes);
        }
        pthread_mutex_unlock(&r->lock);
        if (r->stream) { apply_gain(r->stream); SDL_PutAudioStreamData(r->stream, out, frame_bytes); }
        if (r->wav && r->state == 0) {
            fwrite(out, 1, frame_bytes, r->wav);
            r->wav_bytes += frame_bytes;
            if ((r->elapsed_frames & 255) == 0) { wav_header(r->wav, r->wav_bytes); fflush(r->wav); }
        }
        pthread_mutex_lock(&g_klock);
        kernel_signal(r->event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

static void update_voice(Renderer *r, Voice *v, const uint8_t *p, uint8_t *out) {
    uint8_t is_new = p[8];
    if (is_new) {
        memset(v, 0, sizeof *v);
        v->play_state = 1;
        v->mix_id = -1;
        v->splitter_id = 0xFFFFFFFF;
        for (int i = 0; i < WB_COUNT; i++) v->wb[i].sent = 1;
        v->is_new = 1;
    }
    v->in_use = 1;
    uint8_t ps = p[10];
    if (ps == 0) v->play_state = 0;
    else if (ps == 1) { if (v->play_state != 1) v->play_state = 2; }
    else if (ps == 2) v->play_state = 3;
    v->fmt = p[11];
    memcpy(&v->rate, p + 12, 4);
    uint32_t ch;
    memcpy(&ch, p + 24, 4);
    v->chans = ch > 6 ? 6 : (uint8_t)ch;
    memcpy(&v->pitch, p + 28, 4);
    memcpy(&v->volume, p + 32, 4);
    memcpy(&v->wb_count, p + 60, 4);
    memcpy(&v->ds_addr, p + 72, 8);
    memcpy(&v->mix_id, p + 88, 4);
    memcpy(&v->splitter_id, p + 92, 4);
    memcpy(v->chan_ids, p + 320, 24);
    v->flush += p[345];
    memcpy(&v->behaviour, p + 348, 2);
    if (p[344]) v->drop = 0;
    for (int f = 0; f < 2; f++) {  // BiquadFilterParameter: enable, reserved, s16 numerator[3], denominator[2] (Q14)
        const uint8_t *b = p + 36 + f * 12;
        if (b[0] && !v->bq_on[f]) {
            v->bq_reset[f] = 1;
            if (getenv("LARECOMP_AUDIO_LOG")) {
                int16_t k[5];
                memcpy(k, b + 2, 10);
                LOG("[audren] v%ld biquad%d on: %d %d %d / %d %d\n", (long)(v - r->voice), f, k[0], k[1], k[2], k[3], k[4]);
            }
        }
        v->bq_on[f] = b[0];
        int16_t c[5];
        memcpy(c, b + 2, 10);
        for (int k = 0; k < 5; k++) v->bq[f][k] = c[k] / 16384.0f;
    }
    ChanState *cs0 = &r->chst[v->chan_ids[0] % r->voices];
    if (is_new)
        for (int c = 0; c < v->chans; c++) memset(r->chst[v->chan_ids[c] % r->voices].valid, 0, WB_COUNT);
    for (int i = 0; i < WB_COUNT; i++) {
        const uint8_t *w = p + 96 + i * 0x38;
        WaveBuf *wb = &v->wb[i];
        if (!cs0->valid[i] && wb->sent) wb->addr = 0;
        if (!w[26]) {  // !SentToServer: a new buffer for the DSP
            memcpy(&wb->addr, w, 8);
            memcpy(&wb->size, w + 8, 8);
            memcpy(&wb->start, w + 16, 4);
            memcpy(&wb->end, w + 20, 4);
            wb->loop = w[24];
            wb->eos = w[25];
            memcpy(&wb->loop_count, w + 28, 4);
            memcpy(&wb->ctx, w + 32, 8);
            memcpy(&wb->loop_start, w + 48, 4);
            memcpy(&wb->loop_end, w + 52, 4);
            if ((r->revision >> 24) - '0' < 8) {  // WaveBuffer version 1: loop forever over [start, end)
                wb->loop_count = -1;
                wb->loop_start = wb->loop_end = 0;
            }
            wb->sent = 0;
            static int dumped;
            if (getenv("LARECOMP_AUDIO_DUMPWB") && !dumped && wb->size > 0x100000 && v->fmt == 6 && v->ds_addr) {
                dumped = 1;
                FILE *f = fopen("build/wb_dump.bin", "wb");
                if (f) {
                    uint32_t hdr[4] = {(uint32_t)wb->size, wb->start, wb->end, (uint32_t)(wb->ctx != 0)};
                    fwrite(hdr, 4, 4, f);
                    fwrite((const void *)(uintptr_t)v->ds_addr, 1, 32, f);
                    int16_t ctx[3] = {0};
                    if (wb->ctx) memcpy(ctx, (const void *)(uintptr_t)wb->ctx, 6);
                    fwrite(ctx, 2, 3, f);
                    fwrite((const void *)(uintptr_t)wb->addr, 1, wb->size, f);
                    fclose(f);
                    LOG("[audren] dumped wave buffer %llx (%llx bytes) ds %llx ctx %d %d %d\n", (unsigned long long)wb->addr,
                        (unsigned long long)wb->size, (unsigned long long)v->ds_addr, ctx[0], ctx[1], ctx[2]);
                }
            }
            static int vlog = -1;
            if (vlog < 0) vlog = getenv("LARECOMP_AUDIO_LOG") ? 1 : 0;
            if (vlog)
                LOG("[audren] f%llu v%ld wb%d fmt %u rate %u ch %u pitch %.3f vol %.3f mix %d splitter %u res %u addr %llx size %llx "
                    "[%u,%u) loop %u/%d eos %u ctx %llx ds %llx\n", (unsigned long long)r->elapsed_frames,
                    (long)(v - r->voice), i, v->fmt, v->rate, v->chans, v->pitch, v->volume, v->mix_id,
                    v->splitter_id, v->chan_ids[0],
                    (unsigned long long)wb->addr, (unsigned long long)wb->size, wb->start, wb->end, wb->loop,
                    wb->loop_count, wb->eos, (unsigned long long)wb->ctx, (unsigned long long)v->ds_addr);
            if (vlog && v->ds_addr) {
                const int16_t *k = (const int16_t *)(uintptr_t)v->ds_addr;
                LOG("[audren]   coefs v%ld:", (long)(v - r->voice));
                for (int j = 0; j < 19; j++) LOG(" %d", k[j]);
                LOG("\n");
            }
        }
    }
    // VoiceOutStatus
    if (is_new || v->is_new) {
        memset(out, 0, 0x10);
    } else {
        memcpy(out, &cs0->played, 8);
        memcpy(out + 8, &cs0->consumed, 4);
        out[12] = v->drop;
    }
}

static uint32_t audio_u32(const uint8_t *p) {
    uint32_t value; memcpy(&value, p, sizeof value); return value;
}

static bool audio_route_log_slot(atomic_uint *counter) {
    unsigned count = atomic_load_explicit(counter, memory_order_relaxed);
    while (count < 8) {
        if (atomic_compare_exchange_weak_explicit(counter, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
    return false;
}

static void free_mix_splitters(MixSplitters *s) {
    if (!s) return;
    free(s->info); free(s->dest); free(s->ids); free(s);
}

// Parse a complete candidate before touching persistent routes. IDs are ordered
// per splitter and globally unique: sharing one linked destination across lists
// cannot represent two independent source-channel ordinals.
static bool parse_mix_splitters(Renderer *r, const uint8_t *p, size_t size, MixSplitters **out) {
    unsigned revision = (r->revision >> 24) - '0';
    if (revision < 2 || revision > 11 || !r->splitters || !r->splitter_destinations ||
        r->splitters > AUDREN_SPLITTER_MAX || r->splitter_destinations > AUDREN_SPLITTER_DEST_MAX ||
        size < 0x20 || audio_u32(p) != 0x48444e53) return false;
    uint32_t ni = audio_u32(p + 4), nd = audio_u32(p + 8);
    if (ni > r->splitters || nd > r->splitter_destinations) return false;
    MixSplitters *s = calloc(1, sizeof *s);
    if (!s) return false;
    s->info = calloc(r->splitters, sizeof *s->info);
    s->dest = calloc(r->splitter_destinations, sizeof *s->dest);
    s->ids = calloc(r->splitter_destinations, sizeof *s->ids);
    if (!s->info || !s->dest || !s->ids) { free_mix_splitters(s); return false; }
    const uint8_t *replacement[AUDREN_SPLITTER_MAX] = {0};
    uint32_t counts[AUDREN_SPLITTER_MAX] = {0};
    uint8_t seen_dest[AUDREN_SPLITTER_DEST_MAX] = {0};
    if (r->routing) memcpy(s->dest, r->routing->dest, r->splitter_destinations * sizeof *s->dest);
    size_t at = 0x20;
    for (uint32_t i = 0; i < ni; i++) {
        if (at > size || size - at < 0x1c || audio_u32(p + at) != 0x49444e53) goto bad;
        uint32_t id = audio_u32(p + at + 4), count = audio_u32(p + at + 12);
        if (id >= r->splitters || replacement[id] || count > r->splitter_destinations ||
            count > (size - at - 0x1c) / 4) goto bad;
        replacement[id] = p + at + 0x10;
        counts[id] = revision < 5 && count > r->splitter_destinations / r->splitters ?
                     r->splitter_destinations / r->splitters : count;
        // The ABI includes twelve trailing reserved bytes after each ID list.
        at += 0x1c + (size_t)count * 4;
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < r->splitters; i++) {
        uint32_t count = replacement[i] ? counts[i] : r->routing ? r->routing->info[i].count : 0;
        if (count > r->splitter_destinations - total) goto bad;
        s->info[i] = (MixSplitterInfo){total, count};
        for (uint32_t j = 0; j < count; j++) {
            uint32_t id = replacement[i] ? audio_u32(replacement[i] + j * 4) :
                          r->routing->ids[r->routing->info[i].offset + j];
            if (id >= r->splitter_destinations || seen_dest[id]) goto bad;
            seen_dest[id] = 1;
            s->ids[total++] = id;
        }
    }
    memset(seen_dest, 0, sizeof seen_dest);
    for (uint32_t i = 0; i < nd; i++) {
        if (at > size || size - at < 0x70 || audio_u32(p + at) != 0x44444e53) goto bad;
        uint32_t id = audio_u32(p + at + 4), mix = audio_u32(p + at + 0x68);
        if (id >= r->splitter_destinations || seen_dest[id] || p[at + 0x6c] > 1 ||
            (p[at + 0x6c] && mix != AUDREN_UNUSED_MIX && mix >= r->nmix)) goto bad;
        seen_dest[id] = 1;
        MixSplitterDest *d = &s->dest[id];
        d->mix = (int32_t)mix;
        memcpy(d->volume, p + at + 8, sizeof d->volume);
        if (!d->used && p[at + 0x6c])
            for (uint32_t j = 0; j < MIXBUF_MAX; j++)
                d->previous[j] = isfinite(d->volume[j]) ? d->volume[j] : 0;
        // Finite gains are checked against the staged destination buffer count,
        // not unused ABI slots. Dormant nonfinite slots never enter ramp history.
        d->used = p[at + 0x6c];
        at += 0x70;
    }
    // Only zero alignment padding (at most 15 bytes) may remain.
    if (size - at > 15) goto bad;
    for (; at < size; at++) if (p[at]) goto bad;
    *out = s; return true;
bad:
    free_mix_splitters(s); return false;
}

static bool mix_graph_order(Renderer *r, const Mix *mix, const MixSplitters *split,
                            uint8_t *order, uint32_t *count) {
    uint8_t edge[AUDREN_MIX_MAX][AUDREN_MIX_MAX] = {{0}}, done[AUDREN_MIX_MAX] = {0};
    uint32_t degree[AUDREN_MIX_MAX] = {0}, active = 0;
    if (!r->nmix || r->nmix > AUDREN_MIX_MAX) return false;
    for (uint32_t i = 0; i < r->nmix; i++) {
        if (!mix[i].used) continue;
        active++;
        if (!i) continue; // The final mix is a sink, never a routing source.
        if (mix[i].dest != AUDREN_UNUSED_MIX) {
            int32_t dst = mix[i].dest;
            if (dst < 0 || (uint32_t)dst >= r->nmix) return false;
            if (mix[dst].used) {
                for (uint32_t a = 0; a < mix[i].bufs; a++)
                    for (uint32_t b = 0; b < mix[dst].bufs; b++)
                        if (!isfinite(mix[i].vol[a * MIXBUF_MAX + b])) return false;
                edge[i][dst] = 1;
            }
        } else if (mix[i].dest_splitter != UINT32_MAX) {
            uint32_t id = mix[i].dest_splitter;
            if (!split || id >= r->splitters) return false;
            const MixSplitterInfo *info = &split->info[id];
            for (uint32_t j = 0; j < info->count; j++) {
                const MixSplitterDest *d = &split->dest[split->ids[info->offset + j]];
                if (d->used && d->mix != AUDREN_UNUSED_MIX && mix[d->mix].used) edge[i][d->mix] = 1;
            }
        }
    }
    for (uint32_t i = 0; i < r->nmix; i++)
        for (uint32_t j = 0; j < r->nmix; j++) degree[j] += edge[i][j];
    *count = 0;
    for (uint32_t processed = 0; processed < active; processed++) {
        uint32_t i;
        for (i = 0; i < r->nmix; i++) if (mix[i].used && !done[i] && !degree[i]) break;
        if (i == r->nmix) return false;
        done[i] = 1;
        if (i) order[(*count)++] = (uint8_t)i;
        for (uint32_t j = 0; j < r->nmix; j++) degree[j] -= edge[i][j];
    }
    return true;
}

// Called under the renderer lock. Mix and splitter updates commit together after
// range/float/graph checks and all required allocations have succeeded.
static bool update_mix_routes(Renderer *r, const uint8_t *splitter, size_t splitter_size,
                              const uint8_t *input, size_t size) {
    if (!r->nmix || r->nmix > AUDREN_MIX_MAX) return false;
    Mix *stage = malloc(r->nmix * sizeof *stage);
    if (!stage) return false;
    memcpy(stage, r->mix, r->nmix * sizeof *stage);
    MixSplitters *candidate = NULL;
    uint8_t seen[AUDREN_MIX_MAX] = {0}, order[AUDREN_MIX_MAX];
    uint32_t count = 0;
    if (splitter_size && !parse_mix_splitters(r, splitter, splitter_size, &candidate)) goto bad;
    bool dirty = (r->revision >> 24) - '0' >= 7 && size >= 4 && audio_u32(input) == 0xcafebabe;
    size_t at = dirty ? 0x20 : 0;
    if (size < at || (!dirty && size % 0x930)) goto bad;
    uint32_t n = dirty ? audio_u32(input + 4) : size / 0x930;
    if (n > r->nmix || n > (size - at) / 0x930 || at + (size_t)n * 0x930 != size) goto bad;
    for (uint32_t i = 0; i < n; i++, at += 0x930) {
        const uint8_t *p = input + at;
        uint32_t id = dirty ? audio_u32(p + 16) : i;
        if (id >= r->nmix || seen[id] || p[12] > 1) goto bad;
        seen[id] = 1;
        Mix *m = &stage[id];
        m->used = p[12];
        if (!m->used) continue; // Disabled records do not carry active parameters.
        if (audio_u32(p + 8) > MIXBUF_MAX) goto bad;
        memcpy(&m->volume, p, 4); m->bufs = audio_u32(p + 8);
        m->dest = (int32_t)audio_u32(p + 0x924); m->dest_splitter = audio_u32(p + 0x928);
        memcpy(m->vol, p + 0x24, sizeof m->vol);
        if (!isfinite(m->volume)) goto bad;
    }
    const MixSplitters *routes = candidate ? candidate : r->routing;
    if (routes) for (uint32_t i = 0; i < r->splitter_destinations; i++) {
        const MixSplitterDest *d = &routes->dest[i];
        if (!d->used || d->mix == AUDREN_UNUSED_MIX) continue;
        if (d->mix < 0 || (uint32_t)d->mix >= r->nmix) goto bad;
        if (!stage[d->mix].used) continue;
        for (uint32_t j = 0; j < stage[d->mix].bufs; j++)
            if (!isfinite(d->volume[j]) || !isfinite(d->previous[j])) goto bad;
    }
    if (!mix_graph_order(r, stage, candidate ? candidate : r->routing, order, &count)) goto bad;
    for (uint32_t i = 0; i < r->nmix; i++) if (seen[i] && stage[i].used && !stage[i].buf) {
        stage[i].buf = calloc(MIXBUF_MAX * (size_t)r->sample_count, sizeof(float));
        if (!stage[i].buf) goto bad;
    }
    memcpy(r->mix, stage, r->nmix * sizeof *stage);
    memcpy(r->mix_order, order, count); r->mix_order_count = count;
    if (candidate) { free_mix_splitters(r->routing); r->routing = candidate; }
    if (getenv("LARECOMP_AUDIO_LOG")) {
        for (uint32_t id = 0; id < r->nmix; id++) if (seen[id]) {
            Mix *m = &r->mix[id];
            LOG("[audren] mix %u used %u vol %.3f bufs %u dest %d splitter %u:",
                id, m->used, m->volume, m->bufs, m->dest, m->dest_splitter);
            for (int i = 0; i < MIXBUF_MAX * MIXBUF_MAX; i++)
                if (m->vol[i] != 0) LOG(" %d>%d=%.3f", i / MIXBUF_MAX, i % MIXBUF_MAX, m->vol[i]);
            LOG("\n");
        }
        static atomic_uint parsed_reports, absent_reports, route_reports;
        if (splitter_size && audio_route_log_slot(&parsed_reports))
            LOG("[audren] mix splitter section accepted: bytes %zu infos %u destinations %u ordered submixes %u\n",
                splitter_size, r->splitters, r->splitter_destinations, count);
        if (!splitter_size && r->splitters && audio_route_log_slot(&absent_reports))
            LOG("[audren] mix splitter section absent: retaining previous state %u\n", r->routing != NULL);
        for (uint32_t i = 1; i < r->nmix && r->routing; i++) {
            Mix *m = &r->mix[i];
            if (!m->used || !m->bufs || m->dest != AUDREN_UNUSED_MIX || m->dest_splitter >= r->splitters) continue;
            MixSplitterInfo *info = &r->routing->info[m->dest_splitter];
            for (uint32_t j = 0; j < info->count; j++) {
                uint32_t id = r->routing->ids[info->offset + j];
                MixSplitterDest *dest = &r->routing->dest[id];
                Mix *target = dest->used ? find_mix(r, dest->mix) : NULL;
                if (!target || !audio_route_log_slot(&route_reports)) continue;
                LOG("[audren] mix splitter route accepted: source %u channel %u splitter %u ordinal %u destination %u mix %d source_volume %.6f gains:",
                    i, j % m->bufs, m->dest_splitter, j, id, dest->mix, m->volume);
                for (uint32_t k = 0; k < target->bufs; k++)
                    if (dest->volume[k]) LOG(" %u=%.6f", k, dest->volume[k]);
                LOG("\n");
            }
        }
    }
    free(stage); return true;
bad:
    for (uint32_t i = 0; i < r->nmix; i++) if (stage[i].buf != r->mix[i].buf) free(stage[i].buf);
    free(stage); free_mix_splitters(candidate); return false;
}

// RequestUpdate: input is an UpdateDataHeader + sections; write a matching output.
static void request_update(Renderer *r, IpcReq *req, IpcResp *resp) {
    IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
    if (!in || !out || in->size < 0x40 || out->size < 0x40) return;
    const uint8_t *ip = (const uint8_t *)(uintptr_t)in->addr;
    uint8_t *op = (uint8_t *)(uintptr_t)out->addr;
    uint32_t header[16]; memcpy(header, ip, sizeof header);
    const uint32_t *h = header;
    uint32_t behavior_sz = h[1], pools_sz = h[2], voices_sz = h[3], chans_sz = h[4], effects_sz = h[5];
    uint32_t mixes_sz = h[6], sinks_sz = h[7], perf_sz = h[8];
    size_t forward = 0x40;
    for (unsigned i = 1; i <= 10; i++) {
        if (h[i] > in->size - forward) return;
        forward += h[i];
    }
    if (h[15] != forward || h[15] > in->size ||
        pools_sz % 0x20 || voices_sz % 0x170 || chans_sz % 0x70 ||
        effects_sz % 0xc0 || sinks_sz % 0x140) return;
    // Bound the matching output before acknowledging anything.
    uint64_t needed = 0x40ull + (pools_sz / 0x20) * 0x10ull +
        (voices_sz / 0x170 > r->voices ? r->voices : voices_sz / 0x170) * 0x10ull +
        (effects_sz / 0xc0) * (((r->revision >> 24) - '0' >= 9) ? 0x90ull : 0x10ull) +
        (sinks_sz / 0x140) * 0x20ull + (perf_sz ? 0x10 : 0) + 0xb0 + 0x10;
    if (needed > out->size) return;
    {
        static int dump_n = -1;
        static long count;
        if (dump_n < 0) dump_n = getenv("LARECOMP_AUDIO_DUMP") ? 1 : 0;
        count++;
        if (dump_n && (count == 300 || count == 2000 || count == 6000 || count == 12000)) {
            char path[64];
            snprintf(path, sizeof path, "build/audren_in_%ld.bin", count);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(ip, 1, in->size, f); fclose(f); }
            LOG("[audren] dumped %s (%llu bytes, rev %08x)\n", path, (unsigned long long)in->size, r->revision);
        }
    }
    uint32_t npools = pools_sz / 0x20, nvoices = voices_sz / 0x170, neffects = effects_sz / 0xC0;
    uint32_t nchans = chans_sz / 0x70, nsinks = sinks_sz / 0x140;
    if (nvoices > r->voices) nvoices = r->voices;
    if (nchans > r->voices) nchans = r->voices;
    memset(op, 0, out->size);
    uint32_t *oh = (uint32_t *)op;
    oh[0] = h[0];
    uint32_t o = 0x40;
    pthread_mutex_lock(&r->lock);
    // memory pools: acknowledge attach/detach requests
    const uint8_t *pin = ip + 0x40 + behavior_sz;
    oh[2] = npools * 0x10;
    for (uint32_t i = 0; i < npools && o + 0x10 <= out->size; i++, o += 0x10) {
        uint32_t st = *(const uint32_t *)(pin + i * 0x20 + 0x10);
        *(uint32_t *)(op + o) = st == 4 ? 5 : st == 2 ? 3 : st;
    }
    // voice channel resources
    const uint8_t *cin = pin + pools_sz;
    for (uint32_t i = 0; i < nchans; i++) {
        const uint8_t *c = cin + i * 0x70;
        uint32_t id;
        memcpy(&id, c, 4);
        if (id >= r->voices) continue;
        r->chres[id].used = c[0x64];
        if (c[0x64]) memcpy(r->chres[id].mix, c + 4, sizeof r->chres[id].mix);
    }
    // voices
    const uint8_t *vin = cin + chans_sz;
    oh[3] = r->voices * 0x10;
    for (uint32_t i = 0; i < r->voices; i++) r->voice[i].in_use = 0;
    for (uint32_t i = 0; i < nvoices && o + 0x10 <= out->size; i++, o += 0x10) {
        const uint8_t *v = vin + i * 0x170;
        if (v[9]) update_voice(r, &r->voice[i], v, op + o);
    }
    // effects (not processed): report them as enabled/disabled per request
    const uint8_t *ein = vin + voices_sz;
    // REV9+ uses EffectInfo v2: a 0x10-byte status followed by 0x80 bytes of result state.
    // Keep later output sections aligned to the negotiated revision (Echoes uses REV11).
    uint32_t eout = (r->revision >> 24) - '0' >= 9 ? 0x90 : 0x10;
    oh[5] = neffects * eout;
    for (uint32_t i = 0; i < neffects && o + eout <= out->size; i++, o += eout) {
        const uint8_t *e = ein + i * 0xC0;
        op[o] = e[2] ? 3 : 4;  // EffectState Enabled / Disabled
        if (i < r->effects) effect_update(&r->fx[i], e);
        static uint32_t seen_types;
        if (e[0] && e[0] < 32 && !(seen_types & (1u << e[0]))) {
            seen_types |= 1u << e[0];
            LOG("[audren] effect type %u used%s: slot %u mix %d enabled %u\n", e[0], e[0] == 4 ? "" : " (not implemented)", i,
                *(const int32_t *)(e + 4), e[2]);
        }
        if (e[0] && getenv("LARECOMP_AUDIO_LOG")) {
            static uint8_t last[16][0xC0];
            if (i < 16 && memcmp(last[i], e, 0xC0)) {
                memcpy(last[i], e, 0xC0);
                LOG("[audren] f%llu effect %u:", (unsigned long long)r->elapsed_frames, i);
                for (int k = 0; k < 0x70; k += 4) LOG(" %08x", *(const uint32_t *)(e + k));
                LOG("\n");
            }
        }
    }
    const uint8_t *splitter = ein + effects_sz;
    const uint8_t *min = splitter + h[9];
    const uint8_t *sin = min + mixes_sz;
    if (!update_mix_routes(r, splitter, h[9], min, mixes_sz)) {
        static atomic_uint warnings;
        if (getenv("LARECOMP_AUDIO_LOG") && audio_route_log_slot(&warnings))
            LOG("[audren] rejected mix/splitter routing update: splitter_bytes %u; previous routes retained\n", h[9]);
    }
    // sinks
    oh[7] = nsinks * 0x20;
    for (uint32_t i = 0; i < nsinks; i++, o += 0x20) {
        const uint8_t *s = sin + i * 0x140;
        if (s[0] == 1) {  // device sink
            r->sink_used = s[1];
            uint32_t n;
            memcpy(&n, s + 0x20 + 0x100, 4);
            r->sink_inputs = n > 6 ? 6 : n;
            memcpy(r->sink_input, s + 0x124, 6);
        }
    }
    uint64_t elapsed = r->elapsed_frames;
    pthread_mutex_unlock(&r->lock);
    oh[8] = perf_sz ? 0x10 : 0;
    o += oh[8];
    oh[1] = 0xB0;  // behavior / error info
    o += 0xB0;
    oh[10] = 0x10;  // render info: elapsed frame count
    if (o + 0x10 <= out->size) *(uint64_t *)(op + o) = elapsed;
    o += 0x10;
    oh[15] = o;
    (void)resp;
}

static void renderer_open_output(Renderer *r) {
    if (getenv("LARECOMP_AUDIO_WAV")) {
        r->wav = fopen(getenv("LARECOMP_AUDIO_WAV"), "wb");
        if (r->wav) wav_header(r->wav, 0);
    }
    if (getenv("LARECOMP_NO_AUDIO") && strcmp(getenv("LARECOMP_NO_AUDIO"), "0")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { LOG("[audren] SDL audio init failed: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, (int)r->sample_rate};
    r->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!r->stream) { LOG("[audren] SDL audio open failed: %s\n", SDL_GetError()); return; }
    if (getenv("LARECOMP_AUDIO_GAIN")) SDL_SetAudioStreamGain(r->stream, (float)atof(getenv("LARECOMP_AUDIO_GAIN")));
    SDL_ResumeAudioStreamDevice(r->stream);
}

static void audio_renderer(Service *s, IpcReq *req, IpcResp *resp) {
    Renderer *r = s->state;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = r->sample_rate; resp_size(resp, 4); break;
    case 1: OUT(uint32_t, 0) = r->sample_count; resp_size(resp, 4); break;
    case 2: OUT(uint32_t, 0) = r->mix_buffers; resp_size(resp, 4); break;
    case 3: OUT(uint32_t, 0) = r->state; resp_size(resp, 4); break;
    case 4: case 10: request_update(r, req, resp); break;
    case 5: r->state = 0; break;
    case 6: r->state = 1; break;
    case 7: resp->copy_handles[resp->ncopy++] = r->event_r; break;
    case 8: break;
    case 9: OUT(uint32_t, 0) = 100; resp_size(resp, 4); break;
    default: LOG("[audren] IAudioRenderer cmd %u (ok)\n", req->cmd);
    }
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

/* The shared renderer exposes one stereo output; modern commands enumerate
 * that same configured endpoint using fixed 0x100-byte name records. */
static void audio_output_name(IpcBuf *buffer) {
    static const char name[]="AudioTvOutput";
    memset((void*)(uintptr_t)buffer->addr,0,0x100);
    memcpy((void*)(uintptr_t)buffer->addr,name,sizeof name);
}

static void audio_device(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 6: case 14: { // ListAudioDeviceName(Auto) / ListAudioOutputDeviceName
        IpcBuf *b=out_buf(req,0);
        uint32_t count=b&&b->addr&&b->size>=0x100?1:0;
        if(count)audio_output_name(b);
        OUT(uint32_t,0)=count;resp_size(resp,4);
        if(req->cmd==14)LOG("[audren] output-device list: %u configured endpoint, capacity %llu bytes\n",count,(unsigned long long)(b?b->size:0));
        break;
    }
    case 3: case 10: case 13: { // GetActiveAudioDeviceName(Auto) / GetActiveAudioOutputDeviceName
        IpcBuf *b=out_buf(req,0);
        if(!b||!b->addr||b->size<0x100){resp->result=0x899;break;} // Audio::InsufficientBuffer
        audio_output_name(b);
        if(req->cmd==13)LOG("[audren] active output device: AudioTvOutput\n");
        break;
    }
    case 4: case 11: case 12: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    case 5: OUT(uint32_t, 0) = 2; resp_size(resp, 4); break;  // GetActiveChannelCount
    case 8: OUT(float, 0) = 1.0f; resp_size(resp, 4); break;
    default: break;
    }
}

typedef struct AudioOut AudioOut;
static void audio_track(Renderer *r, AudioOut *a);

// A bounded snapshot of the fixed audren:u request bytes is useful when checking
// the SDK's parameter-dependent work-buffer size. Never follow IPC descriptors or
// dump caller-owned payloads here: req->data is the bounded command metadata.
#define AUDREN_REQUEST_DUMP_MAX 80u
#define AUDREN_REQUEST_SUMMARY_CAP (96u + AUDREN_REQUEST_DUMP_MAX * 2u)
static void audren_request_summary(const IpcReq *req, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!req) {
        snprintf(out, cap, "cmd=<null> data_size=0 captured=0 truncated=0 raw=<null-request>");
        return;
    }
    size_t captured = req->data && req->data_size < AUDREN_REQUEST_DUMP_MAX ? req->data_size :
                      req->data ? AUDREN_REQUEST_DUMP_MAX : 0;
    bool truncated = req->data && req->data_size > captured;
    int prefix = snprintf(out, cap, "cmd=%u data_size=%u captured=%zu truncated=%u raw=",
                          req->cmd, req->data_size, captured, (unsigned)truncated);
    size_t used = prefix < 0 ? 0 : (size_t)prefix < cap ? (size_t)prefix : cap - 1;
    if (!req->data && req->data_size) {
        if (used < cap - 1) {
            int n = snprintf(out + used, cap - used, "<null-data>");
            if (n > 0) used += (size_t)n < cap - used ? (size_t)n : cap - used - 1;
        }
        return;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < captured && used + 2 < cap; ++i) {
        uint8_t byte = req->data[i];
        out[used++] = hex[byte >> 4];
        out[used++] = hex[byte & 15];
    }
    out[used] = '\0';
}

static void audren_log_size_request(const IpcReq *req) {
    char summary[AUDREN_REQUEST_SUMMARY_CAP];
    audren_request_summary(req, summary, sizeof summary);
    LOG("[audren] request %s\n", summary);
}

SERVICE("audren:u", audren_u) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // OpenAudioRenderer(params, work size, aruid, pid, transfer mem, process)
        audren_log_size_request(req);
        Renderer *r = calloc(1, sizeof *r);
        const uint32_t *p = (const uint32_t *)req->data;
        r->sample_rate = p[0]; r->sample_count = p[1]; r->mix_buffers = p[2]; r->submixes = p[3];
        r->voices = p[4]; r->sinks = p[5]; r->effects = p[6]; r->perf_frames = p[7];
        r->splitters = p[9];
        r->splitter_destinations = p[10];
        r->revision = p[12];
        r->state = 1;
        if (r->voices > 1024) r->voices = 1024;
        r->nmix = r->submixes + 1;
        pthread_mutex_init(&r->lock, NULL);
        r->voice = calloc(r->voices, sizeof(Voice));
        r->chres = calloc(r->voices, sizeof(ChanRes));
        r->chst = calloc(r->voices, sizeof(ChanState));
        r->mix = calloc(r->nmix, sizeof(Mix));
        r->fx = calloc(r->effects ? r->effects : 1, sizeof(Effect));
        renderer_open_output(r);
        r->event = kernel_create_event_pair(&r->event_w, &r->event_r);
        pthread_create(&r->thread, NULL, renderer_tick, r);
        audio_track(r, NULL);
        LOG("[audren] renderer %u Hz, %u samples/frame, %u voices, %u mixes, %u mix buffers, %u splitters, %u effects\n",
            r->sample_rate, r->sample_count, r->voices, r->nmix, r->mix_buffers, r->splitters, r->effects);
        ipc_return_object(resp, service_new("IAudioRenderer", audio_renderer, r));
        break;
    }
    case 1: { // GetWorkBufferSize(AudioRendererParameterInternal)
        audren_log_size_request(req);
        uint64_t size = 0;
        enum AudrenWorkSizeResult result = audren_work_size(req->data, req->data_size, &size);
        if (result != AUDREN_WORK_SIZE_OK) {
            // Audio::InvalidRevision / Audio::InvalidArgument. Invalid requests
            // must not return a successful, guessed allocation size.
            resp->result = result == AUDREN_WORK_SIZE_REVISION ? 0xc0299 : 0x70899;
            LOG("[audren] work size rejected: %u\n", (unsigned)result);
            break;
        }
        memcpy(resp->data, &size, sizeof size);
        resp_size(resp, 8);
        LOG("[audren] work size %llu bytes\n", (unsigned long long)size);
        break;
    }
    case 2: case 4: ipc_return_object(resp, service_new("IAudioDevice", audio_device, NULL)); break;
    default: LOG("[audren] cmd %u (ok)\n", req->cmd);
    }
}

// ------------------------------------------------------------------ audout (buffers released in real time)
typedef struct AudioOut {
    uint32_t rate, channels, state;
    uint64_t tags[64], sizes[64], datas[64];
    SDL_AudioStream *stream;
    FILE *wav;
    uint32_t wav_bytes;
    int queued, released_n;
    uint64_t released[64];
    uint32_t ev_r, ev_w;
    KEvent *event;
    uint64_t played;
    pthread_mutex_t lock;
    pthread_t thread;
    volatile bool quit;  // reboot (audio_reset)
} AudioOut;

// Every renderer / audio out of this boot: a reboot stops their threads (they read guest memory and signal this
// boot's kernel events) and closes their output streams.
static Renderer *g_rens[8];
static AudioOut *g_outs[8];
static pthread_mutex_t g_track_lock = PTHREAD_MUTEX_INITIALIZER;
static void audio_track(Renderer *r, AudioOut *a) {
    pthread_mutex_lock(&g_track_lock);
    for (int i = 0; i < 8; i++) {
        if (r && !g_rens[i]) { g_rens[i] = r; r = NULL; }
        if (a && !g_outs[i]) { g_outs[i] = a; a = NULL; }
    }
    pthread_mutex_unlock(&g_track_lock);
}

static void audout_free(AudioOut *a) {
    a->quit = true;
    pthread_join(a->thread, NULL);
    if (a->stream) SDL_DestroyAudioStream(a->stream);
    if (a->wav) fclose(a->wav);
    free(a);
}

// The game closes its IAudioOut after every movie (the title's attract loop opens one per showing): stop the
// thread and the output stream then, or each showing leaves both running and fills g_outs.
static void audout_close(Service *s) {
    AudioOut *a = s->state;
    bool mine = false;
    pthread_mutex_lock(&g_track_lock);
    for (int i = 0; i < 8; i++) if (g_outs[i] == a) { g_outs[i] = NULL; mine = true; }
    pthread_mutex_unlock(&g_track_lock);
    if (!mine) return;  // (a reboot already closed it)
    s->state = NULL;
    audout_free(a);
    LOG("[audout] closed\n");
}

RT_ON_RESET(audio_reset) {
    int n = 0;
    for (int i = 0; i < 8; i++) {
        Renderer *r = g_rens[i];
        if (r) {
            r->quit = true;
            pthread_join(r->thread, NULL);
            if (r->stream) SDL_DestroyAudioStream(r->stream);
            if (r->wav) { wav_header(r->wav, r->wav_bytes); fclose(r->wav); }
            for (uint32_t m = 0; m < r->nmix; m++) free(r->mix[m].buf);
            for (uint32_t e = 0; e < r->effects; e++) {  // each configured reverb's DelayLine buffers (dl_init)
                Reverb *rv = &r->fx[e].rv;
                for (int k = 0; k < 4; k++) { free(rv->fdn[k].buf); free(rv->decay[k].dl.buf); }
                free(rv->pre.buf); free(rv->fc.buf);
            }
            free_mix_splitters(r->routing);
            free(r->voice), free(r->chres), free(r->chst), free(r->mix), free(r->fx);
            free(r);
            g_rens[i] = NULL;
            n++;
        }
        if (g_outs[i]) { audout_free(g_outs[i]); g_outs[i] = NULL; n++; }
    }
    LOG("[audio] reboot: %d outputs closed\n", n);
}

// Buffers are copied into an SDL stream and released once the stream has drained below its target, so the
// stream plays the role of the hardware queue. Without a device, buffers are released in real time.
static void *audout_thread(void *arg) {
    plat_thread_name("audout");
    AudioOut *a = arg;
    uint32_t ch = a->channels ? a->channels : 2;
    int target = 0;
    if (a->stream) {
        SDL_AudioSpec dev;
        int dev_frames = 0;
        if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(a->stream), &dev, &dev_frames) || dev_frames <= 0)
            dev_frames = 1024;
        target = 2 * dev_frames * 2 * (int)ch;  // bytes of s16 at the source rate (close enough at 48 kHz)
        // Eevee's own mixer needs the same queue slack as its working reference.
        // LAS keeps its existing two-period latency unless the explicit override is supplied.
        const TitleAdapter *adapter = title_adapter_selected();
        int milliseconds = adapter ? (int)adapter->audout_min_queue_ms : 0;
        const char *override = getenv("LARECOMP_AUDOUT_MS");
        if (override) { milliseconds = atoi(override); if (milliseconds < 0) milliseconds = 0; if (milliseconds > 1000) milliseconds = 1000; }
        int minimum = (int)((uint64_t)milliseconds * (a->rate ? a->rate : 48000) / 1000) * 2 * (int)ch;
        if (target < minimum) target = minimum;
        LOG("[audout] %u ch: device period %d frames, queue target %d ms\n", ch, dev_frames,
            target / (2 * (int)ch) * 1000 / (int)(a->rate ? a->rate : 48000));
    }
    uint64_t underruns = 0, last_report = 0;
    while (!a->quit) {
        pthread_mutex_lock(&a->lock);
        uint64_t size = 0, tag = 0, data = 0;
        if (a->state == 0 && a->queued) { size = a->sizes[0]; tag = a->tags[0]; data = a->datas[0]; }
        pthread_mutex_unlock(&a->lock);
        if (!size) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); continue; }
        uint64_t frames = size / (2 * ch);
        if (a->stream) {
            apply_gain(a->stream);
            if (a->played && SDL_GetAudioStreamQueued(a->stream) == 0) underruns++;
            uint64_t now = plat_now_ns();
            if (now - last_report > 5000000000ull) {
                if (underruns) LOG("[audout] %u ch: %llu underruns in the last 5 s\n", ch, (unsigned long long)underruns);
                underruns = 0; last_report = now;
            }
            if (data) SDL_PutAudioStreamData(a->stream, (const void *)(uintptr_t)data, (int)size);
            while (SDL_GetAudioStreamQueued(a->stream) > target && !a->quit) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
        } else {
            uint64_t ns = frames * 1000000000ull / (a->rate ? a->rate : 48000);
            struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
            nanosleep(&ts, NULL);
        }
        if (a->wav && data) { fwrite((const void *)(uintptr_t)data, 1, size, a->wav); a->wav_bytes += (uint32_t)size; }
        pthread_mutex_lock(&a->lock);
        if (a->queued && a->tags[0] == tag) {
            if (a->released_n < 64) a->released[a->released_n++] = a->tags[0];
            memmove(a->tags, a->tags + 1, (a->queued - 1) * 8);
            memmove(a->sizes, a->sizes + 1, (a->queued - 1) * 8);
            memmove(a->datas, a->datas + 1, (a->queued - 1) * 8);
            a->queued--;
            a->played += frames;
        }
        pthread_mutex_unlock(&a->lock);
        pthread_mutex_lock(&g_klock);
        kernel_signal(a->event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

static void audio_out(Service *s, IpcReq *req, IpcResp *resp) {
    AudioOut *a = s->state;
    if (getenv("LARECOMP_AUDIO_LOG") && req->cmd != 3 && req->cmd != 7) {
        IpcBuf *ob = out_buf(req, 0);
        LOG("[audout] cmd %u out %llx/%llx\n", req->cmd, ob ? (unsigned long long)ob->addr : 0ull,
            ob ? (unsigned long long)ob->size : 0ull);
    }
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = a->state; resp_size(resp, 4); break;   // GetAudioOutState
    case 1: a->state = 0; break;                                      // Start
    case 2: a->state = 1; break;                                      // Stop
    case 3: case 7: {  // AppendAudioOutBuffer(tag) <- AudioOutBuffer {next, samples, capacity, size, offset}
        IpcBuf *b = in_buf(req, 0);
        uint64_t tag = IN(uint64_t, 0);
        uint64_t size = b && b->size >= 0x28 ? ((uint64_t *)(uintptr_t)b->addr)[3] : 0;
        if (getenv("LARECOMP_AUDIO_LOG") && b)
            LOG("[audout] append tag %llx buf %llx size %llx: %llx %llx %llx %llx %llx\n", (unsigned long long)tag,
                (unsigned long long)b->addr, (unsigned long long)b->size, ((unsigned long long *)(uintptr_t)b->addr)[0],
                ((unsigned long long *)(uintptr_t)b->addr)[1], ((unsigned long long *)(uintptr_t)b->addr)[2],
                ((unsigned long long *)(uintptr_t)b->addr)[3], ((unsigned long long *)(uintptr_t)b->addr)[4]);
        pthread_mutex_lock(&a->lock);
        // AudioOutBuffer {next, samples, capacity, size, offset}
        uint64_t data = b && b->size >= 0x28 ? ((uint64_t *)(uintptr_t)b->addr)[1] + ((uint64_t *)(uintptr_t)b->addr)[4] : 0;
        if (a->queued < 64) {
            a->tags[a->queued] = tag;
            a->sizes[a->queued] = size ? size : 4096;
            a->datas[a->queued] = size ? data : 0;
            a->queued++;
        }
        pthread_mutex_unlock(&a->lock);
        break;
    }
    case 4: resp->copy_handles[resp->ncopy++] = a->ev_r; break;  // RegisterBufferEvent
    case 5: case 8: {  // GetReleasedAudioOutBuffers -> tags
        IpcBuf *b = out_buf(req, 0);
        pthread_mutex_lock(&a->lock);
        int n = a->released_n;
        int max = b ? (int)(b->size / 8) : 0;
        if (n > max) n = max;
        if (b && b->size) memset((void *)(uintptr_t)b->addr, 0, b->size);  // SDK reads entry 0 even when n == 0
        for (int i = 0; i < n; i++) ((uint64_t *)(uintptr_t)b->addr)[i] = a->released[i];
        if (getenv("LARECOMP_AUDIO_LOG") && n) LOG("[audout] released %d (first %llx)\n", n, (unsigned long long)a->released[0]);
        memmove(a->released, a->released + n, (a->released_n - n) * 8);
        a->released_n -= n;
        pthread_mutex_unlock(&a->lock);
        OUT(uint32_t, 0) = (uint32_t)n;
        resp_size(resp, 4);
        break;
    }
    case 6: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 9: OUT(uint32_t, 0) = (uint32_t)a->queued; resp_size(resp, 4); break;
    case 10: OUT(uint64_t, 0) = a->played; resp_size(resp, 8); break;
    case 11: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;
    case 12: break;
    case 13: OUT(float, 0) = 1.0f; resp_size(resp, 4); break;
    default: LOG("[audout] IAudioOut cmd %u (ok)\n", req->cmd);
    }
}

SERVICE("audout:u", audout_u) {
    (void)s;
    switch (req->cmd) {
    case 0: case 2: {  // ListAudioOuts
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 0x100) strcpy((char *)(uintptr_t)b->addr, "DeviceOut");
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    }
    case 1: case 3: {  // OpenAudioOut(rate, channels, aruid, name) -> rate, channels, format, state + IAudioOut
        AudioOut *a = calloc(1, sizeof *a);
        a->rate = IN(uint32_t, 0) ? IN(uint32_t, 0) : 48000;
        a->channels = IN(uint16_t, 4) ? IN(uint16_t, 4) : 2;
        if (a->channels != 1 && a->channels != 2 && a->channels != 6) a->channels = 2;
        a->state = 1;
        pthread_mutex_init(&a->lock, NULL);
        a->event = kernel_create_event_pair(&a->ev_w, &a->ev_r);
        if (getenv("LARECOMP_AUDOUT_WAV")) a->wav = fopen(getenv("LARECOMP_AUDOUT_WAV"), "wb");
        if (!(getenv("LARECOMP_NO_AUDIO") && strcmp(getenv("LARECOMP_NO_AUDIO"), "0")) && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)a->channels, (int)a->rate};
            a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (a->stream) {
                if (getenv("LARECOMP_AUDIO_GAIN")) SDL_SetAudioStreamGain(a->stream, (float)atof(getenv("LARECOMP_AUDIO_GAIN")));
                SDL_ResumeAudioStreamDevice(a->stream);
            }
        }
        pthread_create(&a->thread, NULL, audout_thread, a);
        audio_track(NULL, a);
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size) strncpy((char *)(uintptr_t)b->addr, "DeviceOut", b->size);
        OUT(uint32_t, 0) = a->rate;
        OUT(uint32_t, 4) = a->channels;
        OUT(uint32_t, 8) = 2;  // PCM16
        OUT(uint32_t, 12) = 1; // stopped
        resp_size(resp, 16);
        Service *svc = service_new("IAudioOut", audio_out, a);
        svc->on_close = audout_close;
        ipc_return_object(resp, svc);
        LOG("[audout] opened %u Hz, %u ch\n", a->rate, a->channels);
        break;
    }
    default: LOG("[audout] cmd %u (ok)\n", req->cmd);
    }
}
