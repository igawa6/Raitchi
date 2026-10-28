/* Optional GPU-owned uniform forwarding. Included by exec.c after mirror ownership helpers.
 * Queue transfers use the main command buffer; no CPU read of GPU-written bytes is used. */
typedef struct {
    bool valid;
    uint64_t addr, size, seq, generation;
    VkDescriptorBufferInfo info;
} UniformForward;
static UniformForward uniform_forward[2][17];
static uint64_t uniform_forward_visible_epoch;
static bool uniform_forward_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("LARECOMP_GPU_UNIFORM_FORWARD");
        enabled = !value || atoi(value) != 0;
    }
    return enabled != 0;
}

// Locate any intersection; complete ranges and CPU holes are handled by the
// composition path below. Validate addition before any read or queue mutation.
static MirSlot *uniform_forward_owner(uint64_t addr, uint64_t size) {
    if (!size || addr > UINT64_MAX - size)
        FATAL("[vk] uniform forwarding range %llx+%llx overflows\n",
              (unsigned long long)addr, (unsigned long long)size);
    uint64_t end = addr + size;
    for (int i = 0; i < MIR_SLOTS; ++i) {
        MirSlot *m = &mir_slot[i];
        if (m->used && m->lo < end && addr < m->hi) return m;
    }
    return NULL;
}

// A later CPU upload must also replace any earlier readback in this submission:
// otherwise completion merging could restore the GPU value that the CPU command overrode.
static void uniform_forward_readback(MirSlot *owner, VkCommandBuffer cb) {
    MirSlot *saved[sizeof mir_stored / sizeof mir_stored[0]];
    int count = mir_nstored;
    if (count < 0 || count > (int)(sizeof saved / sizeof saved[0]))
        FATAL("[vk] uniform update stored-range count is invalid\n");
    memcpy(saved, mir_stored, (size_t)count * sizeof saved[0]);
    mir_stored[0] = owner;
    mir_nstored = 1;
    mirror_after_dispatch(cb);
    memcpy(mir_stored, saved, (size_t)count * sizeof saved[0]);
    mir_nstored = count;
}

static bool uniform_forward_prepare(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    if (!uniform_forward_enabled() || !mir_buf) return false;
    uint64_t limit = g_vk.props.limits.maxUniformBufferRange;
    if (size > limit) size = limit;
    if (!uniform_forward_owner(addr, size)) return false;
    uint64_t end = addr + size;
    MirSlot *owners[MIR_SLOTS];
    uint64_t lo[MIR_SLOTS], hi[MIR_SLOTS], offsets[MIR_SLOTS];
    unsigned count = 0;
    for (int i = 0; i < MIR_SLOTS; ++i) {
        MirSlot *m = &mir_slot[i];
        if (!m->used || m->hi <= addr || end <= m->lo) continue;
        uint64_t first = m->lo > addr ? m->lo : addr;
        uint64_t last = m->hi < end ? m->hi : end;
        uint64_t relative = first - m->lo;
        if (m->off > MIR_SIZE || relative > MIR_SIZE - m->off || last - first > MIR_SIZE - m->off - relative)
            FATAL("[vk] uniform %llx+%llx owner %llx..%llx exceeds mirror allocation\n",
                  (unsigned long long)addr, (unsigned long long)size,
                  (unsigned long long)m->lo, (unsigned long long)m->hi);
        for (unsigned j = 0; j < count; ++j) if (lo[j] < last && first < hi[j])
            FATAL("[vk] uniform %llx+%llx has ambiguous GPU owners %llx..%llx and %llx..%llx\n",
                  (unsigned long long)addr, (unsigned long long)size,
                  (unsigned long long)owners[j]->lo, (unsigned long long)owners[j]->hi,
                  (unsigned long long)m->lo, (unsigned long long)m->hi);
        owners[count] = m; lo[count] = first; hi[count] = last;
        offsets[count++] = m->off + relative;
    }
    uint64_t align = g_vk.props.limits.minUniformBufferOffsetAlignment;
    if (!align) align = 1;
    bool direct = count == 1 && lo[0] == addr && hi[0] == end && !(offsets[0] % align);
    if (!direct) for (unsigned i = 0; i < count; ++i)
        if ((offsets[i] | (lo[i] - addr) | (hi[i] - lo[i])) & 3u)
            FATAL("[vk] uniform %llx+%llx GPU intersection %llx..%llx (mirror offset %llx) is not word-aligned\n",
                  (unsigned long long)addr, (unsigned long long)size,
                  (unsigned long long)lo[i], (unsigned long long)hi[i], (unsigned long long)offsets[i]);
    if (in_pass) FATAL("[vk] uniform forwarding preparation inside rendering\n");
    VkCommandBuffer cb = vk_cmd();
    // Earlier draws may still read this owner as a uniform buffer. EndRendering
    // alone does not order those reads before the CPU-diff transfer overwrites.
    vk_full_barrier(cb);
    for (unsigned i = 0; i < count; ++i) {
        MirSlot *owner = owners[i];
        // Recheck each consumer, including CPU edits in the same submission.
        // Unchanged CPU words must not overwrite independently GPU-written words.
        pthread_mutex_lock(&mir_lock);
        owner->checked_seq = UINT64_MAX;
        uint64_t uploaded_before = stat_upload_bytes;
        mirror_cpu_changes(owner, cb);
        owner->last_use = vk_pending_seq();
        pthread_mutex_unlock(&mir_lock);
        if (stat_upload_bytes != uploaded_before) uniform_forward_readback(owner, cb);
    }
    vk_full_barrier(cb); // earlier shader/transfer writes -> uniform reads
    uniform_forward_visible_epoch = uniform_forward_write_epoch;
    if (direct) {
        *out = (VkDescriptorBufferInfo){mir_buf, offsets[0], size};
        return true;
    }
    // Fresh CPU baseline supplies only the holes. Ordered GPU copies replace
    // every GPU-owned intersection without dereferencing GPU-mapped memory.
    VkSpan span = vk_transient_copy((const void *)(uintptr_t)addr, size, align > 256 ? align : 256);
    if (!span.buf || span.off % align) FATAL("[vk] uniform forwarding transient allocation\n");
    vk_full_barrier(cb); // CPU baseline is established before GPU overlays
    for (unsigned i = 0; i < count; ++i) {
        VkBufferCopy copy = {offsets[i], span.off + (lo[i] - addr), hi[i] - lo[i]};
        rec_CmdCopyBuffer(cb, mir_buf, span.buf, 1, &copy);
    }
    vk_full_barrier(cb);
    *out = (VkDescriptorBufferInfo){span.buf, span.off, size};
    return true;
}

// Reuse direct views inside an existing pass only after an already-recorded
// visibility barrier and while no persistent mirror write or CPU edit intervened.
// Graphics storage writers end their pass; compute/transfers also run outside it.
static bool uniform_forward_reuse_graphics(const ShaderInfo *vs, const ShaderInfo *fs) {
    if (!in_pass || mir_graphics_prepared ||
        uniform_forward_visible_epoch != uniform_forward_write_epoch ||
        vs->uses_gmem_store || fs->uses_gmem_store) return false;
    const ShaderInfo *stages[2] = {vs, fs};
    MirSlot *checked[MIR_SLOTS]; unsigned count = 0;
    bool any = false, valid = true;
    pthread_mutex_lock(&mir_lock);
    for (int stage = 0; stage < 2 && valid; ++stage) for (int k = 3; k < 17 && valid; ++k) {
        Range r = st.ubuf[stage][k - 3];
        if (!(stages[stage]->cbuf_mask & (1u << k)) || !r.addr) continue;
        uint64_t size = cbuf_bound_size(stages[stage], k, r);
        uint64_t actual = size > g_vk.props.limits.maxUniformBufferRange ? g_vk.props.limits.maxUniformBufferRange : size;
        if (!actual || r.addr > UINT64_MAX - actual) { valid = false; break; }
        uint64_t end = r.addr + actual;
        MirSlot *owner = NULL;
        for (int i = 0; i < MIR_SLOTS; ++i) {
            MirSlot *m = &mir_slot[i];
            if (!m->used || m->hi <= r.addr || end <= m->lo) continue;
            if (owner || m->lo > r.addr || end > m->hi) { valid = false; break; }
            owner = m;
        }
        if (!valid || !owner) continue;
        any = true;
        uint64_t relative = r.addr - owner->lo, align = g_vk.props.limits.minUniformBufferOffsetAlignment;
        if (!align) align = 1;
        if (owner->off > MIR_SIZE || relative > MIR_SIZE - owner->off || actual > MIR_SIZE - owner->off - relative ||
            (owner->off + relative) % align || !owner->base) { valid = false; break; }
        bool seen = false;
        for (unsigned i = 0; i < count; ++i) seen |= checked[i] == owner;
        if (!seen) {
            if (memcmp((const void *)(uintptr_t)owner->lo, owner->base, owner->hi - owner->lo)) { valid = false; break; }
            checked[count++] = owner;
        }
        UniformForward *entry = &uniform_forward[stage][k];
        *entry = (UniformForward){.valid = true, .addr = r.addr, .size = size,
                                 .seq = vk_pending_seq(), .generation = gcopy_gen,
                                 .info = {mir_buf, owner->off + relative, actual}};
    }
    if (valid && any) for (unsigned i = 0; i < count; ++i) checked[i]->last_use = vk_pending_seq();
    pthread_mutex_unlock(&mir_lock);
    if (!valid || !any) { memset(uniform_forward, 0, sizeof uniform_forward); return false; }
    uniform_forward_graphics_prepared = true;
    return true;
}

static void mirror_uniform_prepare_graphics(const ShaderInfo *vs, const ShaderInfo *fs) {
    memset(uniform_forward, 0, sizeof uniform_forward);
    uniform_forward_graphics_prepared = false;
    if (!uniform_forward_enabled() || !mir_buf) return;
    if (uniform_forward_reuse_graphics(vs, fs)) return;
    const ShaderInfo *stages[2] = {vs, fs};
    // Avoid ending a pass for ordinary CPU-owned uniforms.
    bool owned = false;
    for (int stage = 0; stage < 2; ++stage) for (int k = 3; k < 17; ++k) {
        Range r = st.ubuf[stage][k - 3];
        if (!(stages[stage]->cbuf_mask & (1u << k)) || !r.addr) continue;
        uint64_t size = cbuf_bound_size(stages[stage], k, r);
        if (size > g_vk.props.limits.maxUniformBufferRange) size = g_vk.props.limits.maxUniformBufferRange;
        owned |= uniform_forward_owner(r.addr, size) != NULL;
    }
    if (!owned) return;
    uniform_forward_graphics_prepared = true;
    vk_end_pass();
    for (int stage = 0; stage < 2; ++stage) for (int k = 3; k < 17; ++k) {
        Range r = st.ubuf[stage][k - 3];
        if (!(stages[stage]->cbuf_mask & (1u << k)) || !r.addr) continue;
        uint64_t size = cbuf_bound_size(stages[stage], k, r);
        UniformForward *entry = &uniform_forward[stage][k];
        if (uniform_forward_prepare(r.addr, size, &entry->info)) {
            entry->valid = true; entry->addr = r.addr; entry->size = size;
            entry->seq = vk_pending_seq(); entry->generation = gcopy_gen;
        }
    }
}

static bool mirror_uniform_info(int stage, int k, uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    if (!uniform_forward_enabled() || !mir_buf) return false;
    if (stage == 5) return uniform_forward_prepare(addr, size, out);
    if (stage < 0 || stage >= 2 || k < 3 || k >= 17)
        FATAL("[vk] unsupported uniform forwarding stage or binding\n");
    UniformForward *entry = &uniform_forward[stage][k];
    if (entry->valid && entry->addr == addr && entry->size == size &&
        entry->seq == vk_pending_seq() && entry->generation == gcopy_gen) {
        *out = entry->info;
        return true;
    }
    uint64_t actual = size > g_vk.props.limits.maxUniformBufferRange ? g_vk.props.limits.maxUniformBufferRange : size;
    if (uniform_forward_owner(addr, actual))
        FATAL("[vk] GPU-owned uniform was not prepared before rendering\n");
    return false;
}

// An explicit update is a GPU command even if its payload equals the CPU
// baseline. Record its exact overlapping bytes after earlier shader writes.
static void mirror_uniform_explicit_update(uint64_t addr, const void *data, uint64_t size) {
    if (!uniform_forward_enabled() || !mir_buf || !size) return;
    if (!data || addr > UINT64_MAX - size) FATAL("[vk] uniform update forwarding range overflows\n");
    uint64_t end = addr + size;
    bool any = false;
    for (int i = 0; i < MIR_SLOTS; ++i) {
        const MirSlot *m = &mir_slot[i];
        any |= m->used && m->lo < end && addr < m->hi;
    }
    if (!any) return;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    for (int i = 0; i < MIR_SLOTS; ++i) {
        MirSlot *m = &mir_slot[i];
        if (!m->used || m->hi <= addr || end <= m->lo) continue;
        uint64_t lo = m->lo > addr ? m->lo : addr, hi = m->hi < end ? m->hi : end;
        uint64_t offset = m->off + (lo - m->lo), bytes = hi - lo;
        if ((offset | bytes | (lo - addr)) & 3u)
            FATAL("[vk] explicit uniform mirror update requires word alignment\n");
        if (offset > MIR_SIZE || bytes > MIR_SIZE - offset)
            FATAL("[vk] explicit uniform mirror update allocation bounds\n");
        VkSpan payload = vk_transient_copy((const uint8_t *)data + (lo - addr), bytes, 16);
        VkBufferCopy copy = {payload.off, offset, bytes};
        rec_CmdCopyBuffer(cb, payload.buf, mir_buf, 1, &copy);
        pthread_mutex_lock(&mir_lock);
        memcpy(m->base + (lo - m->lo), (const uint8_t *)data + (lo - addr), bytes);
        m->checked_seq = UINT64_MAX;
        m->last_use = vk_pending_seq();
        pthread_mutex_unlock(&mir_lock);
        uniform_forward_readback(m, cb);
    }
    vk_full_barrier(cb);
    memset(uniform_forward, 0, sizeof uniform_forward);
}
