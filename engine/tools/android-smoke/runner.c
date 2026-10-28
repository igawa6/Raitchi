#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    long pagesize = sysconf(_SC_PAGESIZE);
    void *wanted = (void *)(uintptr_t)0x1000000000ull;
    void *mapped = mmap(wanted, pagesize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    int mapping_ok = mapped != MAP_FAILED && mapped == wanted;
    int collision_ok = 0;
    if (mapping_ok) {
        void *second = mmap(wanted, pagesize, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        collision_ok = second == MAP_FAILED && errno == EEXIST;
        if (second != MAP_FAILED && second != mapped) munmap(second, pagesize);
        *(volatile uint64_t *)mapped = 0x13579;
        mapping_ok &= *(volatile uint64_t *)mapped == 0x13579;
    }
    if (mapped != MAP_FAILED) munmap(mapped, pagesize);
    int all_ok = mapping_ok && collision_ok;
    printf("{\"page_size\":%ld,\"shifted_base_page\":%s,\"mapping_collision_rejected\":%s,\"libraries\":[",
           pagesize, mapping_ok ? "true" : "false", collision_ok ? "true" : "false");
    for (int i = 1; i < argc; i++) {
        void *lib = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
        if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
        void (*metadata)(uint64_t *) = (void (*)(uint64_t *))dlsym(lib, "probe_metadata");
        int (*helpers)(void) = (int (*)(void))dlsym(lib, "probe_helpers");
        if (!metadata || !helpers) return 4;
        uint64_t m[8];
        metadata(m);
        int ok = helpers();
        all_ok &= ok;
        printf("%s{\"source\":\"%s\",\"abi\":%" PRIu64 ",\"cpu_size\":%" PRIu64
               ",\"table_size\":%" PRIu64 ",\"module_capacity\":%" PRIu64
               ",\"guest_shift\":%" PRIu64 ",\"pc_offset\":%" PRIu64
               ",\"vector_offset\":%" PRIu64 ",\"cpu_alignment\":%" PRIu64 ",\"helpers_pass\":%s}",
               i == 1 ? "" : ",", i == 1 ? "las-android" : "letsgo",
               m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], ok ? "true" : "false");
        dlclose(lib);
    }
    puts("]}");
    return all_ok ? 0 : 1;
}
