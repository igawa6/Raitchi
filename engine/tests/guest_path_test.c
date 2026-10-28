#include "runtime/guest_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"path line%d: %s\n",__LINE__,#x);exit(1); } } while (0)
int main(void) {
    char path[128];
    CHECK(shared_guest_path("save/save_1", "/progress.bin", path, sizeof path));
    CHECK(!strcmp(path, "save/save_1/progress.bin"));
    CHECK(shared_guest_path("save/save_1/", "//dir///./file..bin", path, sizeof path));
    CHECK(!strcmp(path, "save/save_1/dir/file..bin"));
    CHECK(shared_guest_path("/", "/name", path, sizeof path) && !strcmp(path, "/name"));
    CHECK(shared_guest_path("save/save_1", "", path, sizeof path) && !strcmp(path, "save/save_1"));
    const char *rejected[] = {"../x", "/../x", "a/../../x", "a/..", "/../../../las-1.0.0/save/save_1/file"};
    for (unsigned i = 0; i < sizeof rejected / sizeof *rejected; ++i) {
        memset(path, 'x', sizeof path);
        CHECK(!shared_guest_path("save/save_1", rejected[i], path, sizeof path));
        CHECK(path[0] == 0);
    }
    CHECK(shared_guest_path("a", "b", path, 4) && !strcmp(path, "a/b"));
    CHECK(!shared_guest_path("a", "b", path, 3) && !path[0]);
    CHECK(shared_guest_path("abc", "", path, 4) && !strcmp(path, "abc"));
    CHECK(!shared_guest_path("abc", "", path, 3) && !path[0]);
    CHECK(!shared_guest_path("", "file", path, sizeof path));
    CHECK(!shared_guest_path(NULL, "file", path, sizeof path));
    CHECK(!shared_guest_path("save", NULL, path, sizeof path));
    CHECK(!shared_guest_path("save", "file", NULL, 0));
    printf("shared guest path: %u checks passed\n",checks);
    return 0;
}
