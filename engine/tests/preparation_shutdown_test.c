/* Production shutdown ordering with controlled UI/worker thread identity. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned events[8], used, checks;
static bool on_main;
static struct { int dev; } g_vk;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %u: %s\n",__LINE__,#x); abort(); } } while (0)
#define LOG(...) ((void)0)
static void event(unsigned n) { CHECK(used < 8); events[used++] = n; }
static void rq_stop(void) { event(1); }
static void rec_quit(void) { event(2); }
static bool SDL_IsMainThread(void) { return on_main; }
void vk_headstart_finish(void) { event(3); }
static void vkDeviceWaitIdle(int dev) { CHECK(dev == 1); event(4); }
static void vk_pipeline_cache_save(void) { event(5); }
#include "preparation_shutdown_production.inc"
int main(int argc, char **argv) {
    CHECK(argc == 2);
    on_main = strcmp(argv[1],"worker") != 0;
    g_vk.dev = strcmp(argv[1],"no-device") != 0;
    backend_quit();
    unsigned before = used;
    backend_quit();
    CHECK(used == before); /* repeated teardown never reruns the drain */
    const unsigned main_order[] = {1,2,3,4,5}, worker_order[] = {1,2,4,5}, empty_order[] = {1,2};
    const unsigned *want = !g_vk.dev ? empty_order : on_main ? main_order : worker_order;
    unsigned count = !g_vk.dev ? 2 : on_main ? 5 : 4;
    CHECK(used == count);
    CHECK(!memcmp(events,want,count * sizeof *events));
    printf("%s: %u shutdown assertions passed\n",argv[1],checks);
    return 0;
}
