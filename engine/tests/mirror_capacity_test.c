/* Exercise the production mirror allocator and merge bodies, not a model. */
static void capacity_wait_hook(void);
#define MIRROR_WAIT_HOOK() capacity_wait_hook()
#define main graphics_existing_main
#include "graphics_mirror_test.c"
#undef main
static unsigned waits;
static void capacity_wait_hook(void) {
    /* Completion/guest-writeback threads must be able to take this lock. */
    CHECK(pthread_mutex_trylock(&mir_lock) == 0);
    CHECK(pthread_mutex_unlock(&mir_lock) == 0);
    waits++;
}
static void full_pool(bool busy) {
    mirror_init();
    uint64_t at = MIR_SNAP;
    const uint64_t units = (MIR_SIZE - MIR_SNAP) / (256 * (MIR_RB + 1));
    for (unsigned i = 0; i < MIR_SLOTS; i++) {
        uint64_t n = i < 120 ? 1 : (units - 120) / 8;
        if (i == MIR_SLOTS - 1) n += (units - 120) % 8;
        MirSlot *m = &mir_slot[i];
        m->used = true; m->lo = HEAP_BASE; m->hi = HEAP_BASE + n * 256;
        m->off = at; m->last_use = busy ? pending : 1;
        at = mir_slot_end(m);
    }
    CHECK(MIR_SIZE - at < 256 * (MIR_RB + 1));
}
static unsigned used_slots(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < MIR_SLOTS; i++) n += mir_slot[i].used;
    return n;
}
static void more_than_eight_idle(void) {
    pending = 2; done = 1; full_pool(false);
    int k = -1; uint64_t off = 0;
    pthread_mutex_lock(&mir_lock);
    CHECK(mirror_slot_reserve(128 * 1024, &k, &off));
    pthread_mutex_unlock(&mir_lock);
    CHECK(k == 0 && off == MIR_SNAP);
    CHECK(used_slots() == 7); /* 120 small victims and then one large one. */
    CHECK(waits == 0); cleanup();
}
static void current_pending_protected(void) {
    pending = 3; done = 0; full_pool(true);
    int k = 47; uint64_t off = 99;
    pthread_mutex_lock(&mir_lock);
    CHECK(!mirror_slot_reserve(128 * 1024, &k, &off));
    pthread_mutex_unlock(&mir_lock);
    CHECK(k == 47 && off == 99 && used_slots() == MIR_SLOTS);
    CHECK(waits == 0 && done == 0); cleanup();
}
static void readback_fixture(uint64_t seq, uint32_t value) {
    MirSlot *m = &mir_slot[0];
    /* Keep its original physical allocation's gap; only four logical bytes
       need a real guest backing range in this controlled fixture. */
    m->hi = HEAP_BASE + 4; m->last_use = seq;
    m->base = calloc(1, 4); m->rb_base[0] = calloc(1, 4);
    CHECK(m->base && m->rb_base[0]);
    m->rb_off[0] = m->off + 256; m->rb_seq[0] = seq;
    memcpy(mapped + m->rb_off[0], &value, 4);
}
static void completed_and_inflight(bool inflight) {
    pending = 3; done = inflight ? 0 : 2; full_pool(true);
    readback_fixture(2, inflight ? 99 : 77);
    int k = -1; uint64_t off = 0;
    unsigned before = waits;
    pthread_mutex_lock(&mir_lock);
    CHECK(mirror_slot_reserve(4, &k, &off));
    pthread_mutex_unlock(&mir_lock);
    CHECK(guest[0] == (inflight ? 99u : 77u));
    CHECK(k == 0 && off == MIR_SNAP && used_slots() == MIR_SLOTS - 1);
    CHECK(waits == before + (unsigned)inflight && done == 2);
    cleanup();
}
static void overflow_and_snapshot_wait(void) {
    pending = 3; done = 0; mirror_init();
    uint64_t bad[] = {0, UINT64_MAX, UINT64_MAX - 254,
                     (MIR_SIZE - MIR_SNAP) / (MIR_RB + 1) + 1};
    pthread_mutex_lock(&mir_lock);
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        int k = 18; uint64_t off = 19;
        CHECK(mirror_slot_alloc(bad[i]) == ~0ull);
        CHECK(!mirror_slot_reserve(bad[i], &k, &off));
        CHECK(k == 18 && off == 19 && !used_slots());
    }
    CHECK(mirror_snap_alloc(0) == ~0ull);
    CHECK(mirror_snap_alloc(UINT64_MAX) == ~0ull);
    CHECK(mir_fh == 0 && mir_head == 0);
    mir_fifo[0].off = 0; mir_fifo[0].end = 256; mir_fifo[0].seq = 2;
    mir_fh = 1; mir_ft = 0; mir_head = MIR_SNAP;
    unsigned before = waits;
    CHECK(mirror_snap_alloc(4) == 0);
    CHECK(waits == before + 1 && done == 2 && mir_head == 256);
    pthread_mutex_unlock(&mir_lock);
    cleanup();
}
int main(void) {
    CHECK(graphics_existing_main() == 0);
    more_than_eight_idle(); current_pending_protected();
    for (unsigned i = 0; i < 20; i++) {
        completed_and_inflight(false); completed_and_inflight(true);
    }
    overflow_and_snapshot_wait();
    printf("{\"state\":\"passed\",\"checks\":%u,\"waits_with_lock_released\":%u,"
           "\"reclaims_more_than_eight\":true,\"pending_preserved\":true,"
           "\"readback_merged_before_reuse\":true}\n", checks, waits);
    return 0;
}
