// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.utils

object Log {
    var gameLaunched = false
    fun debug(message: String) { android.util.Log.d("Raitchi", message) }
    fun warning(message: String) { android.util.Log.w("Raitchi", message) }
    fun info(message: String) { android.util.Log.i("Raitchi", message) }
    fun error(message: String) { android.util.Log.e("Raitchi", message) }
    fun critical(message: String) { android.util.Log.wtf("Raitchi", message) }
}
