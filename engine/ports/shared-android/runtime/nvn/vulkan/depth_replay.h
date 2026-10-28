/* Request-only geometric depth evidence. No game bytes, inferred inputs or
 * production depth-test changes. Included by exec.c after its draw helpers. */
#define DEPTH_REPLAY_MAX_DRAWS 2u
#define DEPTH_REPLAY_MAX_PIXELS (2u * 1024u * 1024u)
#define DEPTH_REPLAY_MAX_BYTES (32ull * 1024ull * 1024ull)
typedef struct { uint64_t vs, fs; unsigned ordinal, count; } DepthReplayConfig;
static bool depth_replay_parse(const char *text, DepthReplayConfig *out) {
    char vs[17], fs[17], tail; unsigned ordinal, count;
    if (!text || !out || strlen(text) > 160 ||
        sscanf(text, "vs=%16[0123456789abcdefABCDEF] fs=%16[0123456789abcdefABCDEF] ordinal=%u count=%u %c",
               vs, fs, &ordinal, &count, &tail) != 4 || strlen(vs) != 16 || strlen(fs) != 16 ||
        !count || count > DEPTH_REPLAY_MAX_DRAWS || ordinal >= 2048 || count > 2048 - ordinal) return false;
    uint64_t vh = strtoull(vs, NULL, 16), fh = strtoull(fs, NULL, 16);
    if (!vh || !fh) return false;
    *out = (DepthReplayConfig){vh, fh, ordinal, count}; return true;
}
static bool depth_replay_extent(uint32_t width, uint32_t height, uint64_t *bytes) {
    if (!width || !height || (uint64_t)width * height > DEPTH_REPLAY_MAX_PIXELS) return false;
    *bytes = (uint64_t)width * height * 4; return true;
}
#ifndef DEPTH_REPLAY_POLICY_ONLY
// The worker owns construction until release-publishing state. The render
// thread alone owns request/selection/capture and retires after GPU completion.
typedef struct {
    _Atomic int state;
    PipeKey original, key;
    VtxLayout vertices;
    VkPipeline pipeline; VkShaderModule fragment;
    VkImage image; VmaAllocation image_mem; VkImageView view;
    VkBuffer output[2]; VmaAllocation memory[2]; const void *mapped[2];
    uint32_t width, height; uint64_t bytes, seq, frame, id;
    unsigned ordinal; bool captured, written;
    const char *error;
    __typeof__(ss) recorded;
    uint32_t count, first, instances, base_instance; int32_t base_vertex;
    bool indexed;
    VkImage reference_image; VkImageView reference_view;
    uint64_t reference_guest;
} DepthReplayJob;
static struct {
    DepthReplayConfig config;
    DepthReplayJob jobs[DEPTH_REPLAY_MAX_DRAWS];
    uint64_t id, frame, capture_frame;
    unsigned seen, phase; // 0 idle, 1 discover/build, 2 armed, 3 retire
    int enabled;
} depth_replay = {.frame = UINT64_MAX, .enabled = -1};

static void depth_replay_status(const char *status) {
    FILE *f = fopen("depth-replay.status", "w");
    if (f) { fprintf(f, "request=%llu status=%s\n", (unsigned long long)depth_replay.id, status); fclose(f); }
}
static void depth_replay_destroy(DepthReplayJob *j) {
    if (j->pipeline) vkDestroyPipeline(g_vk.dev, j->pipeline, NULL);
    if (j->fragment) vkDestroyShaderModule(g_vk.dev, j->fragment, NULL);
    if (j->view) vkDestroyImageView(g_vk.dev, j->view, NULL);
    if (j->image) vmaDestroyImage(g_vk.vma, j->image, j->image_mem);
    for (int i = 0; i < 2; ++i) if (j->output[i]) vmaDestroyBuffer(g_vk.vma, j->output[i], j->memory[i]);
    memset(j, 0, sizeof *j);
}
static void depth_replay_build(void *arg) {
    DepthReplayJob *j = arg;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_R32_SFLOAT, &fp);
    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((fp.optimalTilingFeatures & needed) != needed) { j->error = "R32-MIN-unsupported"; goto failed; }
    j->fragment = vk_compile_glsl("#version 450\nlayout(location=0) out float depth_value;\nvoid main(){depth_value=gl_FragCoord.z;}\n",
                                  STAGE_FRAGMENT, "depth-replay");
    if (!j->fragment) { j->error = "fragment-build-failed"; goto failed; }
    PipeCI ci; pipe_ci_fill(&ci, &j->key);
    ci.stages[1].module = j->fragment;
    ci.cba[0] = (VkPipelineColorBlendAttachmentState){.blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .colorBlendOp = VK_BLEND_OP_MIN, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .alphaBlendOp = VK_BLEND_OP_MIN, .colorWriteMask = VK_COLOR_COMPONENT_R_BIT};
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &ci.pi, NULL, &j->pipeline) != VK_SUCCESS) {
        j->error = "pipeline-build-failed"; goto failed;
    }
    VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT, .extent = {j->width, j->height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo allocation = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &image, &allocation, &j->image, &j->image_mem, NULL) != VK_SUCCESS) {
        j->error = "image-allocation-failed"; goto failed;
    }
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = j->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32_SFLOAT,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &view, NULL, &j->view) != VK_SUCCESS) { j->error = "image-view-failed"; goto failed; }
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = j->bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    allocation = (VmaAllocationCreateInfo){.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, .preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT};
    for (int i = 0; i < 2; ++i) {
        VmaAllocationInfo info;
        if (vmaCreateBuffer(g_vk.vma, &buffer, &allocation, &j->output[i], &j->memory[i], &info) != VK_SUCCESS || !info.pMappedData) {
            j->error = "readback-allocation-failed"; goto failed;
        }
        j->mapped[i] = info.pMappedData;
    }
    atomic_store_explicit(&j->state, 1, memory_order_release); return;
failed:
    atomic_store_explicit(&j->state, 2, memory_order_release);
}
static void depth_replay_write(DepthReplayJob *j) {
    mkdir("build", 0755);
    char path[192]; bool complete = true;
    const char *names[2] = {"nearest", "reference"};
    for (int i = 0; i < 2; ++i) {
        bool valid = vmaInvalidateAllocation(g_vk.vma, j->memory[i], 0, j->bytes) == VK_SUCCESS && j->mapped[i];
        snprintf(path, sizeof path, "build/depth-replay-%llu-%llu-%u-%s.bin", (unsigned long long)j->id,
                 (unsigned long long)j->frame, j->ordinal, names[i]);
        FILE *f = valid ? fopen(path, "wb") : NULL;
        if (!f) complete = false;
        else { size_t n = fwrite(j->mapped[i], 1, j->bytes, f); if (fclose(f) || n != j->bytes) complete = false; }
    }
    snprintf(path, sizeof path, "build/depth-replay-%llu-%llu-%u.json", (unsigned long long)j->id,
             (unsigned long long)j->frame, j->ordinal);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "{\"schema\":1,\"status\":\"%s\",\"mode\":\"nearest-depth-MIN\",\"source\":\"GPU-replay-already-bound-inputs\","
            "\"request\":%llu,\"frame\":%llu,\"ordinal\":%u,\"seq\":%llu,\"width\":%u,\"height\":%u,\"bytes_per_plane\":%llu,"
            "\"vs_hash\":\"%016llx\",\"fs_hash\":\"%016llx\",\"depth_format\":%d,\"effective_depth_may_include_projection_writes\":true,"
            "\"occluded_depth_differences_are_not_errors\":true,\"no_fresh_CPU_input_reads\":true,"
            "\"indexed\":%s,\"count\":%u,\"first\":%u,\"base_vertex\":%d,\"base_instance\":%u,\"instances\":%u,"
            "\"reference_image\":\"%llx\",\"reference_view\":\"%llx\",\"reference_guest\":\"%llx\","
            "\"viewport\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],\"scissor\":[%d,%d,%u,%u],"
            "\"cull\":%u,\"front_face\":%u,\"bias_enabled\":%u,\"bias\":[%.9g,%.9g,%.9g],\"descriptor_sets\":[\"%llx\",\"%llx\"],\"dynamic_offsets\":[",
            complete ? "captured" : "readback-write-failed", (unsigned long long)j->id, (unsigned long long)j->frame, j->ordinal,
            (unsigned long long)j->seq, j->width, j->height, (unsigned long long)j->bytes,
            (unsigned long long)depth_replay.config.vs, (unsigned long long)depth_replay.config.fs, j->key.depth,
            j->indexed ? "true" : "false", j->count, j->first, j->base_vertex, j->base_instance, j->instances,
            (unsigned long long)(uintptr_t)j->reference_image, (unsigned long long)(uintptr_t)j->reference_view,
            (unsigned long long)j->reference_guest, j->recorded.vp.x, j->recorded.vp.y, j->recorded.vp.width,
            j->recorded.vp.height, j->recorded.vp.minDepth, j->recorded.vp.maxDepth, j->recorded.sc.offset.x,
            j->recorded.sc.offset.y, j->recorded.sc.extent.width, j->recorded.sc.extent.height,
            (unsigned)j->recorded.cull, (unsigned)j->recorded.ff, (unsigned)j->recorded.dbe,
            j->recorded.db[0], j->recorded.db[1], j->recorded.db[2],
            (unsigned long long)(uintptr_t)j->recorded.ds[0], (unsigned long long)(uintptr_t)j->recorded.ds[1]);
        for (uint32_t i = 0; i < j->recorded.ndyn; ++i) fprintf(f, "%s%u", i ? "," : "", j->recorded.dyn[i]);
        fprintf(f, "],\"vertex_buffers\":[");
        for (unsigned s = 0; s < 17; ++s) fprintf(f, "%s[\"%llx\",%llu]", s ? "," : "",
            (unsigned long long)(uintptr_t)j->recorded.vb[s], (unsigned long long)j->recorded.vo[s]);
        fprintf(f, "],\"index_buffer\":[\"%llx\",%llu,%u]}\n", (unsigned long long)(uintptr_t)j->recorded.ib,
            (unsigned long long)j->recorded.io, (unsigned)j->recorded.it); fclose(f);
    }
    j->written = true;
}
static void depth_replay_poll(void) {
    if (depth_replay.enabled < 0) { const char *e = getenv("LARECOMP_MATERIAL_PROBE"); depth_replay.enabled = e && atoi(e) != 0; }
    if (!depth_replay.enabled) return;
    uint64_t frame = vk_frame_counter();
    if (frame == depth_replay.frame) return;
    if (depth_replay.phase) {
        bool busy = false, failed = false, pending = false; const char *failure = "build-failed";
        for (unsigned i = 0; i < depth_replay.config.count; ++i) {
            DepthReplayJob *j = &depth_replay.jobs[i];
            int state = atomic_load_explicit(&j->state, memory_order_acquire);
            busy |= j->original.prog && state == 0; failed |= state == 2;
            if (state == 2 && j->error) failure = j->error;
            if (j->captured && !j->written && j->seq <= vk_completed_seq()) depth_replay_write(j);
            pending |= j->captured && !j->written;
        }
        if (failed) { depth_replay_status(failure); depth_replay.phase = 3; }
        if (depth_replay.phase == 2 && frame != depth_replay.capture_frame) {
            for (unsigned i = 0; i < depth_replay.config.count; ++i)
                if (!depth_replay.jobs[i].captured) depth_replay_status("selected-draw-missing-or-unsupported");
            depth_replay.phase = 3;
        }
        if (depth_replay.phase == 3 && !busy && !pending) {
            for (unsigned i = 0; i < depth_replay.config.count; ++i) depth_replay_destroy(&depth_replay.jobs[i]);
            depth_replay.phase = 0;
        }
    }
    if (frame != depth_replay.frame) {
        depth_replay.frame = frame; depth_replay.seen = 0;
        if (depth_replay.phase == 1) {
            bool ready = true;
            for (unsigned i = 0; i < depth_replay.config.count; ++i)
                ready &= atomic_load_explicit(&depth_replay.jobs[i].state, memory_order_acquire) == 1;
            if (ready) { depth_replay.phase = 2; depth_replay.capture_frame = frame; depth_replay_status("armed"); }
        }
    }
    if (depth_replay.phase) return;
    struct stat info;
    if (lstat("depth-replay.request", &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 160) return;
    FILE *f = fopen("depth-replay.request", "r"); if (!f) return;
    char text[162] = {0}; size_t n = fread(text, 1, sizeof text - 1, f); fclose(f);
    if (unlink("depth-replay.request")) return;
    depth_replay.id++;
    if (!n || !depth_replay_parse(text, &depth_replay.config)) { depth_replay_status("invalid-request"); return; }
    depth_replay.phase = 1; depth_replay.seen = 0;
    depth_replay_status("discovering-private-pipelines");
}
static int depth_replay_select(const Prog *p, const PipeKey *key, const VtxLayout *vertices) {
    if (!depth_replay.phase || depth_replay.phase == 3 || p->fp[0].hash != depth_replay.config.vs || p->fp[1].hash != depth_replay.config.fs) return -1;
    unsigned ordinal = depth_replay.seen++;
    if (ordinal < depth_replay.config.ordinal || ordinal - depth_replay.config.ordinal >= depth_replay.config.count) return -1;
    unsigned slot = ordinal - depth_replay.config.ordinal;
    uint64_t bytes;
    bool supported = !g_debug_frame && pass_samples == 1 && st.rt.depth && pass_depth_attachment_view &&
        !ss.dw && ss.dt && ss.dop == VK_COMPARE_OP_EQUAL && !ss.sten && !ss.dbe && !key->a2c &&
        !p->vi->uses_gmem_store && !p->fi->uses_gmem_store && !p->fi->uses_discard && !p->fi->writes_depth &&
        ss.topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && depth_replay_extent(pass_w, pass_h, &bytes);
    if (!supported) { depth_replay_status("unsupported-selected-draw"); depth_replay.phase = 3; return -1; }
    DepthReplayJob *j = &depth_replay.jobs[slot];
    if (depth_replay.phase == 1 && !j->original.prog) {
        j->original = *key; j->key = *key; j->vertices = *vertices;
        j->key.ncolor = 1; memset(j->key.color, 0, sizeof j->key.color); j->key.color[0] = VK_FORMAT_R32_SFLOAT;
        j->key.blend_on = 0; memset(j->key.write_mask, 0, sizeof j->key.write_mask); j->key.write_mask[0] = VK_COLOR_COMPONENT_R_BIT;
        j->key.a2c = false; j->width = pass_w; j->height = pass_h; j->bytes = bytes; j->id = depth_replay.id; j->ordinal = ordinal;
        run_job(depth_replay_build, j);
    }
    if (depth_replay.phase != 2 || atomic_load_explicit(&j->state, memory_order_acquire) != 1 || j->captured) return -1;
    if (memcmp(&j->original, key, sizeof *key) || memcmp(&j->vertices, vertices, sizeof *vertices) || j->width != pass_w || j->height != pass_h) {
        depth_replay_status("selected-pipeline-state-changed"); depth_replay.phase = 3; return -1;
    }
    return (int)slot;
}

// This emitter consumes already-bound Vulkan objects only. It never calls back
// into draw/program/descriptor/mirror preparation and never waits or submits.
static void depth_replay_emit(VkCommandBuffer cb, DepthReplayJob *j) {
    VkBufferImageCopy reference = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {j->width, j->height, 1}};
    vk_full_barrier(cb);
    rec_CmdCopyImageToBuffer(cb, j->reference_image, VK_IMAGE_LAYOUT_GENERAL, j->output[1], 1, &reference);
    VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = j->view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    color.clearValue.color.float32[0] = INFINITY;
    VkRenderingAttachmentInfo depth = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = j->reference_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo render = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {j->width, j->height}}, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &color, .pDepthAttachment = &depth,
        .pStencilAttachment = j->key.stencil ? &depth : NULL};
    rec_CmdBeginRendering(cb, &render);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, j->pipeline);
    rec_CmdSetViewport(cb, 0, 1, &j->recorded.vp); rec_CmdSetScissor(cb, 0, 1, &j->recorded.sc);
    rec_CmdSetCullMode(cb, j->recorded.cull); rec_CmdSetFrontFace(cb, j->recorded.ff);
    rec_CmdSetPrimitiveTopology(cb, j->recorded.topo); rec_CmdSetPrimitiveRestartEnable(cb, j->recorded.pre);
    rec_CmdSetDepthBiasEnable(cb, j->recorded.dbe); rec_CmdSetDepthBias(cb, j->recorded.db[1], j->recorded.db[2], j->recorded.db[0]);
    rec_CmdSetDepthTestEnable(cb, VK_TRUE); rec_CmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    rec_CmdSetDepthWriteEnable(cb, VK_FALSE); rec_CmdSetStencilTestEnable(cb, VK_FALSE);
    if (g_vk.vids) set_vertex_input(cb, &j->vertices);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, j->key.prog->layout, 0, 2, j->recorded.ds,
                              j->recorded.ndyn, j->recorded.ndyn ? j->recorded.dyn : NULL);
    // Higher GMEM sets and push constants remain bound: exact same layout.
    for (unsigned s = 0; s < 17; ++s) if (j->vertices.used_streams & (1u << s))
        rec_CmdBindVertexBuffers(cb, s, 1, &j->recorded.vb[s], &j->recorded.vo[s]);
    if (j->indexed) {
        rec_CmdBindIndexBuffer(cb, j->recorded.ib, j->recorded.io, j->recorded.it);
        rec_CmdDrawIndexed(cb, j->count, j->instances, 0, j->base_vertex, j->base_instance);
    } else rec_CmdDraw(cb, j->count, j->instances, j->first, j->base_instance);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    VkBufferImageCopy nearest = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {j->width, j->height, 1}};
    rec_CmdCopyImageToBuffer(cb, j->image, VK_IMAGE_LAYOUT_GENERAL, j->output[0], 1, &nearest);
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    rec_CmdPipelineBarrier2(cb, &dependency);
}
static void depth_replay_capture(int slot, bool indexed, uint32_t count, uint32_t first, int32_t base_vertex,
                                  uint32_t base_instance, uint32_t instances, unsigned runs) {
    if (slot < 0 || runs != 1 || depth_replay.phase != 2) return;
    DepthReplayJob *j = &depth_replay.jobs[slot];
    TexBackend *b = st.rt.depth ? st.rt.depth->backend : NULL;
    if (!b || !(b->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || b->levels != 1 || b->layers != 1 ||
        b->width < j->width || b->height < j->height || b->samples != 1 || !pass_depth_attachment_view ||
        (b->format != VK_FORMAT_D32_SFLOAT_S8_UINT && b->format != VK_FORMAT_D32_SFLOAT && b->format != VK_FORMAT_D24_UNORM_S8_UINT)) {
        depth_replay_status("unsupported-effective-depth"); depth_replay.phase = 3; return;
    }
    j->recorded = ss; j->indexed = indexed; j->count = count; j->first = first; j->base_vertex = base_vertex;
    j->base_instance = base_instance; j->instances = instances;
    j->reference_image = b->image; j->reference_view = pass_depth_attachment_view; j->reference_guest = st.rt.depth->addr;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_image_init_layout(cb, j->image, VK_IMAGE_ASPECT_COLOR_BIT);
    depth_replay_emit(cb, j);
    in_pass = false; st.rt_dirty = true; ss_forget();
    j->captured = true; j->seq = vk_pending_seq(); j->frame = vk_frame_counter();
    depth_replay_status("GPU-readbacks-queued");
}
#endif
