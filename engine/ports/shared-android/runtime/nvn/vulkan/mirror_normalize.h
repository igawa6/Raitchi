/* Conservative admission of one authoritative mirror owner per guest word.
 * New overlap only: synchronize old GPU uses before replacing their offsets.
 * Ordinary reads and contained ranges retain their existing owner without waits. */
static uint64_t mirror_normalize_count, mirror_normalize_wait_ns;
typedef struct { uint64_t lo, hi; bool requested, write; } MirrorComponent;

static MirSlot *mirror_normalize_allocate(uint64_t lo, uint64_t hi, const uint8_t *seed) {
    int index = 0; uint64_t at = ~0ull, bytes = hi - lo;
    if (!mirror_slot_reserve(bytes, &index, &at))
        FATAL("[vk] normalized mirror allocation cannot fit %llx..%llx\n", (unsigned long long)lo, (unsigned long long)hi);
    MirSlot *m = &mir_slot[index];
    *m = (MirSlot){.used = true, .lo = lo, .hi = hi, .off = at, .last_use = vk_pending_seq(), .base = malloc(bytes)};
    if (!m->base) FATAL("[vk] normalized mirror base allocation failed\n");
    uint64_t stride = (bytes + 255) & ~255ull;
    for (int j = 0; j < MIR_RB; ++j) {
        m->rb_off[j] = at + (uint64_t)(j + 1) * stride;
        m->rb_base[j] = malloc(bytes);
        if (!m->rb_base[j]) FATAL("[vk] normalized mirror readback allocation failed\n");
    }
    memcpy(m->base, seed ? seed : (const void *)(uintptr_t)lo, bytes);
    memcpy(mir_ptr + at, m->base, bytes);
    vmaFlushAllocation(g_vk.vma, mir_mem, at, bytes);
    uniform_forward_write_epoch++;
    stat_upload_bytes += bytes;
    return m;
}

static void mirror_normalize_ranges(const MirRange *ranges, int nranges) {
    if (!uniform_forward_enabled() || !nranges) return;
    if (nranges < 0 || nranges > 64) FATAL("[vk] mirror normalization range count\n");
    mirror_init();
    // Rebuild after each admission: allocation may retire an unrelated idle
    // owner, and a synchronized overlap advances the submission sequence.
    for (int progress = 0; progress < 2 * MIR_SLOTS + 64; ++progress) {
        MirrorComponent components[MIR_SLOTS + 64]; int n = 0;
        pthread_mutex_lock(&mir_lock);
        for (int i = 0; i < MIR_SLOTS; ++i) if (mir_slot[i].used) {
            const MirSlot *m = &mir_slot[i];
            for (int j = 0; j < i; ++j) if (mir_slot[j].used && mir_slot[j].lo < m->hi && m->lo < mir_slot[j].hi)
                FATAL("[vk] mirror normalization requires a fresh process: overlapping active owners %llx..%llx and %llx..%llx\n",
                      (unsigned long long)mir_slot[j].lo, (unsigned long long)mir_slot[j].hi,
                      (unsigned long long)m->lo, (unsigned long long)m->hi);
            components[n++] = (MirrorComponent){m->lo, m->hi, false, false};
        }
        for (int i = 0; i < nranges; ++i) {
            if (ranges[i].hi <= ranges[i].lo || (ranges[i].lo | ranges[i].hi) & 3u)
                FATAL("[vk] mirror normalization invalid word range\n");
            components[n++] = (MirrorComponent){ranges[i].lo, ranges[i].hi, true, !ranges[i].ro};
        }
        for (int i = 1; i < n; ++i) {
            MirrorComponent value = components[i]; int j = i;
            while (j && components[j - 1].lo > value.lo) { components[j] = components[j - 1]; --j; }
            components[j] = value;
        }
        int merged = 0;
        for (int i = 0; i < n; ++i) {
            if (merged && components[i].lo < components[merged - 1].hi) {
                MirrorComponent *m = &components[merged - 1];
                if (components[i].hi > m->hi) m->hi = components[i].hi;
                m->requested |= components[i].requested; m->write |= components[i].write;
            } else components[merged++] = components[i];
        }
        MirrorComponent target = {0}; int owners[MIR_SLOTS], count = 0;
        for (int i = 0; i < merged; ++i) if (components[i].requested) {
            MirrorComponent c = components[i]; int hits = 0, only = -1;
            for (int j = 0; j < MIR_SLOTS; ++j) if (mir_slot[j].used && mir_slot[j].lo < c.hi && c.lo < mir_slot[j].hi) { hits++; only = j; }
            if (!hits && !c.write) continue;
            if (hits == 1 && mir_slot[only].lo == c.lo && mir_slot[only].hi == c.hi) {
                mir_slot[only].last_use = vk_pending_seq(); continue;
            }
            target = c;
            for (int j = 0; j < MIR_SLOTS; ++j) if (mir_slot[j].used && mir_slot[j].lo < c.hi && c.lo < mir_slot[j].hi) owners[count++] = j;
            break;
        }
        if (!target.hi) { pthread_mutex_unlock(&mir_lock); return; }
        uint64_t bytes = target.hi - target.lo;
        if (bytes > (MIR_SIZE - MIR_SNAP) / (MIR_RB + 1))
            FATAL("[vk] normalized mirror union %llx..%llx exceeds capacity\n",
                  (unsigned long long)target.lo, (unsigned long long)target.hi);
        if (!count) {
            mirror_normalize_allocate(target.lo, target.hi, NULL);
            pthread_mutex_unlock(&mir_lock); continue;
        }
        // Save CPU edits before renderer_flush can merge pending GPU writes.
        uint8_t *saved = malloc(bytes), *dirty = calloc(bytes / 4, 1), *seed = malloc(bytes);
        if (!saved || !dirty || !seed) FATAL("[vk] mirror normalization snapshot allocation failed\n");
        memcpy(saved, (const void *)(uintptr_t)target.lo, bytes);
        for (int i = 0; i < count; ++i) {
            const MirSlot *m = &mir_slot[owners[i]];
            uint64_t start = (m->lo - target.lo) / 4, words = (m->hi - m->lo) / 4;
            const uint32_t *cpu = (const uint32_t *)(saved + start * 4), *base = (const uint32_t *)m->base;
            for (uint64_t w = 0; w < words; ++w) dirty[start + w] = cpu[w] != base[w];
        }
        pthread_mutex_unlock(&mir_lock);
        vk_end_pass();
        uint64_t start_ns = SDL_GetTicksNS();
        renderer_flush(); // no lock held: queued users of ALL old physical offsets finish
        uint64_t elapsed = SDL_GetTicksNS() - start_ns;
        pthread_mutex_lock(&mir_lock);
        uint64_t done = vk_completed_seq();
        memcpy(seed, (const void *)(uintptr_t)target.lo, bytes); // current CPU gaps
        for (int i = 0; i < count; ++i) {
            MirSlot *m = &mir_slot[owners[i]];
            if (!m->used || m->lo < target.lo || m->hi > target.hi || m->gpu_seq > done)
                FATAL("[vk] mirror normalization GPU ownership changed while synchronizing\n");
            for (int j = 0; j < MIR_RB; ++j) if (m->rb_seq[j] > done)
                FATAL("[vk] mirror normalization readback still pending\n");
            uint64_t offset = m->lo - target.lo, length = m->hi - m->lo;
            // Completed GPU memory is read only after explicit invalidation.
            vmaInvalidateAllocation(g_vk.vma, mir_mem, m->off, length);
            memcpy(seed + offset, mir_ptr + m->off, length);
            uint32_t *out = (uint32_t *)(seed + offset);
            const uint32_t *was = (const uint32_t *)(saved + offset), *cpu = (const uint32_t *)(uintptr_t)m->lo, *base = (const uint32_t *)m->base;
            for (uint64_t w = 0; w < length / 4; ++w) {
                if (dirty[offset / 4 + w]) out[w] = was[w];
                if (cpu[w] != base[w]) out[w] = cpu[w]; // a newer CPU edit surviving the wait
            }
            memcpy((void *)(uintptr_t)m->lo, seed + offset, length);
            mirror_slot_drop(m); // GPU completion now permits physical reuse
        }
        mirror_normalize_allocate(target.lo, target.hi, seed);
        pthread_mutex_unlock(&mir_lock);
        free(saved); free(dirty); free(seed);
        guest_copy_flush();
        memset(uniform_forward, 0, sizeof uniform_forward);
        uniform_forward_graphics_prepared = false;
        mirror_normalize_count++; mirror_normalize_wait_ns += elapsed;
        if (mirror_normalize_count <= 32)
            LOG("[vk] mirror normalization #%llu %llx..%llx (%d owners), GPU wait %.3f ms (total %.3f ms)\n",
                (unsigned long long)mirror_normalize_count, (unsigned long long)target.lo, (unsigned long long)target.hi,
                count, elapsed / 1000000.0, mirror_normalize_wait_ns / 1000000.0);
    }
    FATAL("[vk] mirror normalization did not converge\n");
}

static void mirror_normalize_compute(const ShaderInfo *si) {
    if (!uniform_forward_enabled() || !vk_gmem_mirror() || !si->uses_gmem_map) return;
    MirRange ranges[32]; int n = 0;
    for (int i = 0; i < 32; ++i) {
        bool uniform = i >= 16;
        Range r = uniform ? st.ubuf[5][i - 16] : st.sbuf[5][i]; uint64_t lo, hi;
        if (!r.addr || !r.size) continue;
        if (!mirror_guest_range(r.addr, r.size, &lo, &hi)) {
            if (!uniform && si->uses_gmem_store) FATAL("[vk] invalid storing compute range before normalization\n");
            continue;
        }
        ranges[n++] = (MirRange){lo, hi, uniform || !si->uses_gmem_store};
    }
    mirror_normalize_ranges(ranges, n);
}
static void mirror_normalize_graphics(const ShaderInfo *vs, const ShaderInfo *fs) {
    if (!uniform_forward_enabled() || !vk_gmem_mirror()) return;
    const ShaderInfo *stages[2] = {vs, fs}; MirRange ranges[32]; int n = 0;
    for (int stage = 0; stage < 2; ++stage) if (stages[stage]->uses_gmem_map)
        for (int i = 0; i < 16; ++i) {
            Range r = st.sbuf[stage][i]; uint64_t lo, hi;
            if (!r.addr || !r.size) continue;
            if (!mirror_guest_range(r.addr, r.size, &lo, &hi)) {
                if (stages[stage]->uses_gmem_store) FATAL("[vk] invalid storing graphics range before normalization\n");
                continue;
            }
            ranges[n++] = (MirRange){lo, hi, !stages[stage]->uses_gmem_store};
        }
    mirror_normalize_ranges(ranges, n);
}
