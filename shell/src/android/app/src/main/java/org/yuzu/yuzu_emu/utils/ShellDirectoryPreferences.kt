// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.SharedPreferences
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import org.yuzu.yuzu_emu.model.DirectoryType
import org.yuzu.yuzu_emu.model.GameDir

internal class ShellDirectoryPreferences(private val preferences: SharedPreferences) {
    fun getGameDirs(): Array<GameDir> {
        val entries = readArray(GAME_DIRS)
        return (0 until entries.length()).mapNotNull { index ->
            val item = entries.optJSONObject(index) ?: return@mapNotNull null
            val uri = item.opt("uri") as? String ?: return@mapNotNull null
            if (uri.isBlank()) return@mapNotNull null
            val type = runCatching {
                DirectoryType.valueOf(item.optString("type", DirectoryType.GAME.name))
            }.getOrDefault(DirectoryType.GAME)
            GameDir(uri, item.optBoolean("deepScan", true), type)
        }.toTypedArray()
    }

    fun setGameDirs(dirs: Array<GameDir>) {
        val entries = JSONArray()
        dirs.forEach { dir ->
            entries.put(JSONObject().put("uri", dir.uriString)
                .put("deepScan", dir.deepScan).put("type", dir.type.name))
        }
        writeArray(GAME_DIRS, entries)
    }

    fun getExternalContentDirs(): Array<String> {
        val entries = readArray(EXTERNAL_DIRS)
        return (0 until entries.length()).mapNotNull { index ->
            (entries.opt(index) as? String)?.takeIf { it.isNotBlank() }
        }.toTypedArray()
    }

    fun setExternalContentDirs(dirs: Array<String>) = writeArray(EXTERNAL_DIRS, JSONArray(dirs.toList()))

    private fun readArray(key: String): JSONArray = try {
        JSONArray(preferences.getString(key, "[]") ?: "[]")
    } catch (_: JSONException) {
        JSONArray()
    } catch (_: ClassCastException) {
        JSONArray()
    }

    private fun writeArray(key: String, entries: JSONArray) {
        check(preferences.edit().putString(key, entries.toString()).commit()) {
            "Could not persist Raitchi directories"
        }
    }

    private companion object {
        const val GAME_DIRS = "game_dirs_v1"
        const val EXTERNAL_DIRS = "external_dirs_v1"
    }
}
