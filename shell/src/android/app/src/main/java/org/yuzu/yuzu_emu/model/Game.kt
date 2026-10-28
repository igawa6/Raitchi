// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.model

import android.content.Intent
import android.net.Uri
import android.os.Parcelable
import java.util.HashSet
import kotlinx.parcelize.Parcelize
import kotlinx.serialization.Serializable
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.activities.RaitchiLaunchActivity
import org.yuzu.yuzu_emu.utils.DirectoryInitialization
import org.yuzu.yuzu_emu.utils.FileUtil

@Parcelize
@Serializable
class Game(
    val title: String = "",
    val path: String,
    val programId: String = "",
    val developer: String = "",
    var version: String = "",
    val isHomebrew: Boolean = false,
    val adapterId: String = "",
    val compatible: Boolean = false,
    val compatibilityReason: String = "Compatibility has not been checked",
    /** No adapter yet, but the engine can attempt a generic start that may close unexpectedly. */
    val tryable: Boolean = false
) : Parcelable {
    val launchable get() = compatible || tryable

    val keyAddedToLibraryTime get() = "${path}_AddedToLibraryTime"
    val keyLastPlayedTime get() = "${path}_LastPlayed"

    val settingsName: String
        get() {
            val programIdLong = programId.toLongOrNull() ?: 0L
            return if (programIdLong == 0L) {
                FileUtil.getFilename(Uri.parse(path))
            } else {
                "0" + programIdLong.toString(16).uppercase()
            }
        }

    val programIdHex: String
        get() {
            val programIdLong = programId.toLongOrNull() ?: 0L
            return if (programIdLong == 0L) {
                "0"
            } else {
                "0" + programIdLong.toString(16).uppercase()
            }
        }

    val addonDir: String
        get() = DirectoryInitialization.userDirectory + "/load/" + programIdHex + "/"

    val launchIntent: Intent
        get() = Intent(YuzuApplication.appContext, RaitchiLaunchActivity::class.java).apply {
            action = Intent.ACTION_VIEW
            data = Uri.parse(path)
            putExtra("adapterId", adapterId)
        }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false

        other as Game

        if (title != other.title) return false
        if (path != other.path) return false
        if (programId != other.programId) return false
        if (developer != other.developer) return false
        if (version != other.version) return false
        if (isHomebrew != other.isHomebrew) return false
        if (adapterId != other.adapterId) return false
        if (compatible != other.compatible) return false
        if (compatibilityReason != other.compatibilityReason) return false
        if (tryable != other.tryable) return false

        return true
    }

    override fun hashCode(): Int {
        var result = title.hashCode()
        result = 31 * result + path.hashCode()
        result = 31 * result + programId.hashCode()
        result = 31 * result + developer.hashCode()
        result = 31 * result + version.hashCode()
        result = 31 * result + isHomebrew.hashCode()
        result = 31 * result + adapterId.hashCode()
        result = 31 * result + compatible.hashCode()
        result = 31 * result + compatibilityReason.hashCode()
        result = 31 * result + tryable.hashCode()
        return result
    }

    companion object {
        val extensions: Set<String> = HashSet(
            listOf("xci", "nsp")
        )
    }
}
