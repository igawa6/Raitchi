/* Compiled with the production queue_run/queue_push and discovery coordinator.
 * Only translation/driver calls use controlled synthetic work. */
static void *parallel_append(void *unused) {
    (void)unused;usleep(15000);
    uint8_t payload[40]={0x10};NvnDiscoveryRecipe r={.rec={.kind='C',.local={3,2,1}},.vkey={.glsl=1},.code={payload,NULL},.code_bytes={32,0},.constant_bytes={8,0}};
    r.rec.vs=shader_code_hash(payload,sizeof payload);
    pthread_mutex_lock(&discovery_store_lock);
    /* Force the next append to move its underlying storage. Replay must keep
     * only indices/copies, never pointers into this reallocating byte vector. */
    uint8_t *old=discovery_store.bytes;uint8_t *copy=malloc(discovery_store.size);CHECK(copy);memcpy(copy,old,discovery_store.size);
    discovery_store.bytes=copy;discovery_store.capacity=discovery_store.size;free(old);
    seed(&r);pthread_mutex_unlock(&discovery_store_lock);return NULL;
}
static int parallel_test(const char *mode) {
    bool serial=!strcmp(mode,"real_serial"),cancel=!strcmp(mode,"real_cancel"),append=!strcmp(mode,"real_append");
    if(!strcmp(mode,"real_one"))builder_count=1;
    if(!strcmp(mode,"real_fail_program"))fail_program=true;
    if(!strcmp(mode,"real_fail_module"))fail_module=true;
    if(!strcmp(mode,"real_fail_pipeline"))fail_pipeline=true;
    if(!strcmp(mode,"real_fail_compute"))fail_compute=true;
    if(!strcmp(mode,"real_bounds")) {
        DiscoveryReplayStats zero=discovery_replay_schedule(1,0),large=discovery_replay_schedule(1,DISCOVERY_REPLAY_WINDOW+1),many=discovery_replay_schedule(DISCOVERY_RECORDS+1,1);
        CHECK(zero.skipped==1&&large.skipped==1&&many.skipped==DISCOVERY_RECORDS+1&&!hs_total&&!hs_done);
        cleanup();printf("{\"assertions\":%u}\n",atomic_load(&checks));return 0;
    }
    // Six independent programs, one shared-program state variant, and a
    // semantically different recipe that yields the same current pipeline.
    for(unsigned i=0;i<6;i++) {
        NvnDiscoveryRecipe r=graphics(false);vs[0]=(uint8_t)(0x30+i);r.rec.vs=shader_code_hash(vs,sizeof vs);seed(&r);
        if(!i){r.rec.samples=2;seed(&r);r.rec.samples=1;r.rec.attrib[0]++;seed(&r);}
    }
    unsigned ncompute=!strcmp(mode,"real_budget")?3:2;
    for(unsigned i=0;i<ncompute;i++){NvnDiscoveryRecipe r=compute();r.rec.local[0]=i+1;seed(&r);}
    size_t count=nvn_discovery_count(&discovery_store);CHECK(count==8+ncompute);
    pthread_t appended={0};if(append)CHECK(!pthread_create(&appended,NULL,parallel_append,NULL));
    uint64_t start=SDL_GetTicksNS();DiscoveryReplayStats stats={0};
    if(cancel) {
        vk_headstart_prepare(progress,cancel_now);CHECK(hs_requested&&!hs_finished);
        vk_headstart_finish();CHECK(hs_finished&&discovery_stopping);
    } else stats=discovery_replay_schedule(count,serial?1:DISCOVERY_REPLAY_WINDOW);
    uint64_t elapsed=SDL_GetTicksNS()-start;
    if(append)CHECK(!pthread_join(appended,NULL));
    CHECK(translate_calls==6&&pipeline_calls==(fail_program||fail_module?0:7)&&compute_calls==ncompute);
    CHECK(hs_done==hs_total&&hs_total==6+(fail_program||fail_module?0:7)+(int)ncompute);
    CHECK(!active_builders&&!active_modules&&!active_translations&&max_translations==1);
    CHECK(max_builders<=builder_count);
    // Every prewarm job, including graphics modules, obeys bg_max2 once
    // gameplay begins. The dedicated translator remains serial.
    for (;;) {
        pthread_mutex_lock(&g_jobs.lock);bool busy=g_jobs.head||g_jobs.bhead||g_jobs.bg_running;pthread_mutex_unlock(&g_jobs.lock);
        if(!busy)break;SDL_Delay(1);
    }
    unsigned programs=fail_program||fail_module?0:6,pipes=fail_program||fail_module||fail_pipeline?0:7,computes=fail_compute?0:ncompute;
    CHECK(hs_cache_record_count==programs+pipes+computes);
    if(!strcmp(mode,"real_budget"))CHECK(stats.deferred>0&&stats.computes==3&&!stats.skipped);
    if(append)CHECK(nvn_discovery_count(&discovery_store)==count+1&&hs_cs_n==ncompute);
    if(serial)CHECK(max_builders==1&&!pipeline_overlap);
    if(!strcmp(mode,"real_parallel"))CHECK(max_builders>=2&&pipeline_overlap>0);
    if(!strcmp(mode,"real_one"))CHECK(max_builders==1);
    uint64_t strict_size=hs_cache_file_bytes;unsigned prior_total=hs_total;
    // All references, including failures, terminate. A repeat neither queues
    // work nor appends current records (the concurrent append belongs to the
    // next launch and is deliberately outside this frozen source count).
    DiscoveryReplayStats repeat=discovery_replay_schedule(count,DISCOVERY_REPLAY_WINDOW);
    CHECK(!repeat.programs&&!repeat.pipelines&&!repeat.computes&&hs_total==(int)prior_total&&hs_done==hs_total&&strict_size==hs_cache_file_bytes);
    discovery_finish();cleanup();
    printf("{\"assertions\":%u,\"max_builders\":%u,\"max_translations\":%u,\"pipeline_overlap\":%u,\"elapsed_ms\":%.3f,\"deferrals\":%u}\n",atomic_load(&checks),atomic_load(&max_builders),atomic_load(&max_translations),atomic_load(&pipeline_overlap),(double)elapsed/1e6,stats.deferred);
    return 0;
}
