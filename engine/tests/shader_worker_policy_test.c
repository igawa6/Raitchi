#include <assert.h>
#include <stdio.h>
#include "nvn/shader_worker_policy.h"

int main(void) {
    assert(shader_worker_count(8, 16, true, NULL) == 7);
    assert(shader_worker_count(4, 8, true, NULL) == 3);
    assert(shader_worker_count(2, 8, true, NULL) == 1);
    assert(shader_worker_count(1, 8, true, NULL) == 1);
    assert(shader_worker_count(32, 32, true, NULL) == 8);
    assert(shader_worker_count(0, 8, true, NULL) == 7);
    assert(shader_worker_count(0, 0, true, NULL) == 1);
    assert(shader_worker_count(32, 32, false, NULL) == 16);
    assert(shader_worker_count(8, 8, true, "1") == 1);
    assert(shader_worker_count(8, 8, true, "16") == 16);
    assert(shader_worker_count(8, 8, true, "999999999999999999999") == 7);
    assert(shader_worker_count(8, 8, true, "999") == 16);
    assert(shader_worker_count(8, 8, true, "0") == 7);
    assert(shader_worker_count(8, 8, true, "-1") == 7);
    assert(shader_worker_count(8, 8, true, "4x") == 7);
    assert(shader_worker_count(8, 8, true, "") == 7);
    assert(shader_background_capacity(7, true) == 7);
    assert(shader_background_capacity(7, false) == 2);
    assert(shader_background_capacity(1, false) == 1);
    assert(shader_background_capacity(0, true) == 1);
    puts("shader worker policy: 20 checks passed");
}
