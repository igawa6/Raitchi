// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.Context
import android.net.Uri
import com.lasrecomp.android.RaitchiMetadata
import java.io.File
import org.yuzu.yuzu_emu.YuzuApplication

/** Shell-side metadata facade backed by the Raitchi container reader. */
object GameMetadata {
    private val metadata = mutableMapOf<String, RaitchiMetadata.GameInfo>()
    private val icons = mutableMapOf<String, ByteArray>()

    @Synchronized
    fun inspect(uri: Uri, context: Context = YuzuApplication.appContext): RaitchiMetadata.GameInfo {
        val key = uri.toString()
        return metadata[key] ?: RaitchiMetadata.inspect(context, uri).also { metadata[key] = it }
    }

    fun getInfo(path: String): RaitchiMetadata.GameInfo = inspect(Uri.parse(path))

    fun getIsValid(path: String): Boolean = runCatching { inspect(Uri.parse(path)) }.isSuccess
    fun getTitle(path: String): String = inspect(Uri.parse(path)).title
    fun getProgramId(path: String): String = inspect(Uri.parse(path)).programId
    fun getDeveloper(path: String): String = inspect(Uri.parse(path)).developer
    fun getVersion(path: String, reload: Boolean): String {
        if (reload) invalidate(path)
        return inspect(Uri.parse(path)).version
    }
    fun getIsHomebrew(path: String): Boolean = false

    @Synchronized
    fun getIcon(path: String): ByteArray {
        return icons[path] ?: File(inspect(Uri.parse(path)).iconPath).readBytes()
            .also { icons[path] = it }
    }

    @Synchronized
    fun resetMetadata() {
        metadata.clear()
        icons.clear()
    }

    @Synchronized
    private fun invalidate(path: String) {
        metadata.remove(path)
        icons.remove(path)
    }
}
