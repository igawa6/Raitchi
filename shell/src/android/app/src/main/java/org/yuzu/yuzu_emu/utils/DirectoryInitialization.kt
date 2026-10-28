// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import org.yuzu.yuzu_emu.YuzuApplication
import java.io.File

object DirectoryInitialization {
    var areDirectoriesReady = false
        private set
    private var userPath: String? = null
    val userDirectory: String?
        get() { check(areDirectoriesReady); return userPath }

    fun start() {
        if (areDirectoriesReady) return
        val directory = File(YuzuApplication.appContext.filesDir, "shell")
        check(directory.isDirectory || directory.mkdirs()) { "Cannot create Raitchi shell storage" }
        userPath = directory.canonicalPath
        NativeConfig.initializeGlobalConfig()
        areDirectoriesReady = true
    }
}
