#pragma once
#include "nxpkg.h"
/* Opens one package-backed RomFS with independent FD/cache ownership. Read env
 * LARECOMP_PACKAGE_URI (content:// or local path), fallback PACKAGE_PATH, and
 * LARECOMP_PACKAGE_KEYS. Android content access uses this game's SDL Activity.
 * Revalidates selected adapterprogram and identical prepared ExeFS before RomFS.
 * Returns NULL with an actionable nonsecret reason; no fatal or process state.
 * Close via nx_romfs_close only after all file users have joined. */
NxRomfs *shared_package_open_romfs(char *why,size_t why_size);
