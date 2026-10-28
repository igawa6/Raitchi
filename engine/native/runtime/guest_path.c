#include "guest_path.h"
#include <string.h>

int shared_guest_path(const char *root, const char *guest, char *out, size_t capacity) {
    if (out && capacity) out[0] = 0;
    if (!root || !*root || !guest || !out || !capacity) return 0;
    size_t used = strlen(root);
    while (used > 1 && root[used - 1] == '/') --used;
    if (used >= capacity) return 0;
    memcpy(out, root, used);
    out[used] = 0;
    const char *p = guest;
    while (*p) {
        while (*p == '/') ++p;
        const char *start = p;
        while (*p && *p != '/') ++p;
        size_t count = (size_t)(p - start);
        if (!count || (count == 1 && start[0] == '.')) continue;
        if ((count == 2 && start[0] == '.' && start[1] == '.') || count >= capacity - used) {
            out[0] = 0; return 0;
        }
        if (out[used - 1] != '/') {
            if (used + 1 >= capacity) { out[0] = 0; return 0; }
            out[used++] = '/';
        }
        if (count >= capacity - used) { out[0] = 0; return 0; }
        memcpy(out + used, start, count);
        used += count;
        out[used] = 0;
    }
    return 1;
}
