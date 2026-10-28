/* Initial30fps policy from Let's Go hle/fps60.c, pinned96894fd8e9ea20c1b38cdea53105ec2a335df620.
 * Copyright (c) 2026 raulzito12345rrs. MIT: ../../runtime/LICENSE-LetsGo. */
#include "../title_adapter.h"
#include <stdlib.h>
/* Pinned rt_scene_tick returns1 for every original30fps update. Its alternate
 * result requires optional all60+scene timing state, which this initial adapter
 * deliberately does not enable. This is the actual baseline timing policy. */
__attribute__((visibility("default"))) int rt_scene_tick(void) {
    const TitleAdapter *adapter = title_adapter_selected();
    if (!adapter || adapter->id != TITLE_EEVEE) abort();
    return 1;
}
