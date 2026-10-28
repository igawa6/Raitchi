// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.Context
import android.content.SharedPreferences
import androidx.appcompat.app.AppCompatDelegate
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.model.GameDir
import com.lasrecomp.android.RaitchiShellBridge
import com.lasrecomp.android.SettingsStore

/** Kotlin settings facade: app UI preferences plus Raitchi's validated per-adapter settings. */
object NativeConfig {
    private const val PREFS = "raitchi_shell_config"
    private const val PREF_SAVED_REVISION = "settings_saved_revision"

    private val appContext get() = YuzuApplication.appContext
    private val appPreferences: SharedPreferences
        get() = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    @Volatile private var activeAdapterId: String? = null

    private val directoryPreferences get() = ShellDirectoryPreferences(appPreferences)

    private val intUiDefaults = mapOf(
        "theme" to 0,
        "theme_mode" to AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM,
        "static_theme_color" to 8,
        "app_language" to 0,
        "control_scale" to 0,
        "control_opacity" to 100,
        "vertical_alignment" to 0,
        "overlay_grid_size" to 24,
        "input_overlay_auto_hide" to 0
    )

    private val booleanUiDefaults = mapOf(
        "black_backgrounds" to false,
        "show_input_overlay" to true,
        "dpad_slide" to false,
        "touchscreen" to true,
        "haptic_feedback" to true,
        "overlay_snap_to_grid" to true,
        "enable_input_overlay_auto_hide" to false,
        "hide_overlay_on_controller_input" to false,
        "invert_confirm_back_controller_buttons" to false
    )

    @Synchronized
    fun initializeGlobalConfig() {
        val options = RaitchiShellBridge.settingOptions(null)
        options.forEach { option -> RaitchiShellBridge.getSetting(appContext, null, option.key) }
        intUiDefaults.forEach { (key, default) -> appPreferences.getInt(key, default) }
        booleanUiDefaults.forEach { (key, default) -> appPreferences.getBoolean(key, default) }
    }

    @Synchronized
    fun unloadGlobalConfig() {
        flushAppPreferences()
    }

    @Synchronized
    fun reloadGlobalConfig() {
        initializeGlobalConfig()
    }

    @Synchronized
    fun saveGlobalConfig() {
        validateEngineScope(null)
        flushAppPreferences()
    }

    @Synchronized
    fun initializePerGameConfig(programId: String, fileName: String, adapterId: String = "") {
        require(programId.isNotBlank()) { "Game program ID is missing" }
        require(fileName.isNotBlank()) { "Game file name is missing" }
        require(adapterId.isNotBlank()) { "This game has no Raitchi settings adapter" }
        val options = RaitchiShellBridge.settingOptions(adapterId)
        require(options.isNotEmpty()) { "This game has no settings supported by Raitchi" }
        activeAdapterId = adapterId
    }

    @Synchronized
    fun isPerGameConfigLoaded(): Boolean = activeAdapterId != null

    @Synchronized
    fun savePerGameConfig() {
        val adapterId = activeAdapterId ?: unsupported("No per-game settings scope is open")
        validateEngineScope(adapterId)
    }

    @Synchronized
    fun unloadPerGameConfig() {
        activeAdapterId = null
    }

    @Synchronized
    fun resetCurrentConfig() {
        val adapterId = activeAdapterId
        SettingsStore(appContext).reset(adapterId)
        if (adapterId == null) {
            val editor = appPreferences.edit()
            intUiDefaults.forEach { (key, value) -> editor.putInt(key, value) }
            booleanUiDefaults.forEach { (key, value) -> editor.putBoolean(key, value) }
            checkCommit(editor.commit())
            flushAppPreferences()
        }
    }

    fun getString(key: String, needsGlobal: Boolean = false): String {
        val intDefault = intUiDefaults[key]
        if (intDefault != null) return appPreferences.getInt(key, intDefault).toString()
        val boolDefault = booleanUiDefaults[key]
        if (boolDefault != null) return if (appPreferences.getBoolean(key, boolDefault)) "1" else "0"
        return RaitchiShellBridge.getSetting(appContext, settingScope(needsGlobal), checkedEngineOption(key, settingScope(needsGlobal)).key)
    }

    fun setString(key: String, value: String) {
        when {
            key in intUiDefaults -> {
                val parsed = value.toIntOrNull() ?: unsupported("Invalid integer value for $key")
                checkCommit(appPreferences.edit().putInt(key, parsed).commit())
            }
            key in booleanUiDefaults -> setBoolean(key, parseBooleanSetting(value.lowercase(), key))
            else -> {
                val adapterId = activeAdapterId
                checkedEngineOption(key, adapterId)
                RaitchiShellBridge.setSetting(appContext, adapterId, key, value)
            }
        }
    }

    fun getBoolean(key: String, needsGlobal: Boolean = false): Boolean {
        val default = booleanUiDefaults[key]
        if (default != null) return appPreferences.getBoolean(key, default)
        return parseBooleanSetting(getString(key, needsGlobal), key)
    }

    fun setBoolean(key: String, value: Boolean) {
        if (key in booleanUiDefaults) {
            checkCommit(appPreferences.edit().putBoolean(key, value).commit())
            return
        }
        val option = checkedEngineOption(key, activeAdapterId)
        val stored = when {
            "1" in option.values && "0" in option.values -> if (value) "1" else "0"
            "true" in option.values && "false" in option.values -> value.toString()
            else -> unsupported("Setting $key is not a boolean option")
        }
        RaitchiShellBridge.setSetting(appContext, activeAdapterId, key, stored)
    }

    fun getByte(key: String, needsGlobal: Boolean = false): Byte = getString(key, needsGlobal).toByteOrNull()
        ?: unsupported("Setting $key is not a byte value")
    fun setByte(key: String, value: Byte) = setString(key, value.toString())
    fun getShort(key: String, needsGlobal: Boolean = false): Short = getString(key, needsGlobal).toShortOrNull()
        ?: unsupported("Setting $key is not a short value")
    fun setShort(key: String, value: Short) = setString(key, value.toString())
    fun getUnsignedShort(key: String, needsGlobal: Boolean = false): Int = getString(key, needsGlobal).toIntOrNull()
        ?.takeIf { it in 0..65535 } ?: unsupported("Setting $key is not an unsigned short value")
    fun setUnsignedShort(key: String, value: Int) {
        require(value in 0..65535) { "Value is outside the unsigned short range" }
        setString(key, value.toString())
    }
    fun getInt(key: String, needsGlobal: Boolean = false): Int = getString(key, needsGlobal).toIntOrNull()
        ?: unsupported("Setting $key is not an integer option")
    fun setInt(key: String, value: Int) = setString(key, value.toString())
    fun getFloat(key: String, needsGlobal: Boolean = false): Float = getString(key, needsGlobal).toFloatOrNull()
        ?: unsupported("Setting $key is not a float option")
    fun setFloat(key: String, value: Float) {
        require(value.isFinite()) { "Setting $key requires a finite value" }
        setString(key, value.toString())
    }
    fun getLong(key: String, needsGlobal: Boolean = false): Long = getString(key, needsGlobal).toLongOrNull()
        ?: unsupported("Setting $key is not a long option")
    fun setLong(key: String, value: Long) = setString(key, value.toString())

    fun getIsRuntimeModifiable(key: String): Boolean {
        if (key in intUiDefaults || key in booleanUiDefaults) return true
        checkedEngineOption(key, activeAdapterId)
        return false
    }
    fun getPairedSettingKey(key: String): String {
        if (key !in intUiDefaults && key !in booleanUiDefaults) checkedEngineOption(key, activeAdapterId)
        return ""
    }
    fun getIsSwitchable(key: String): Boolean {
        if (key in intUiDefaults || key in booleanUiDefaults) return false
        checkedEngineOption(key, activeAdapterId)
        return true
    }
    fun usingGlobal(key: String): Boolean {
        if (key in intUiDefaults || key in booleanUiDefaults) return true
        val adapterId = activeAdapterId ?: return true
        checkedEngineOption(key, adapterId)
        return !SettingsStore(appContext).hasOverride(adapterId, key)
    }

    fun setGlobal(key: String, global: Boolean) {
        val adapterId = activeAdapterId ?: unsupported("There is no per-game settings scope")
        checkedEngineOption(key, adapterId)
        if (global) {
            SettingsStore(appContext).clearOverride(adapterId, key)
        } else {
            RaitchiShellBridge.setSetting(appContext, adapterId, key,
                RaitchiShellBridge.getSetting(appContext, null, key))
        }
    }

    fun getIsSaveable(key: String): Boolean {
        if (key in intUiDefaults || key in booleanUiDefaults) return true
        checkedEngineOption(key, activeAdapterId)
        return true
    }

    fun getDefaultToString(key: String): String {
        intUiDefaults[key]?.let { return it.toString() }
        booleanUiDefaults[key]?.let { return if (it) "true" else "false" }
        return checkedEngineOption(key, activeAdapterId).initial
    }

    fun getGameDirs(): Array<GameDir> = directoryPreferences.getGameDirs()

    fun setGameDirs(dirs: Array<GameDir>) = directoryPreferences.setGameDirs(dirs)

    fun addGameDir(dir: GameDir) {
        val current = getGameDirs().toMutableList()
        if (current.none { it.uriString == dir.uriString && it.type == dir.type }) current.add(dir)
        setGameDirs(current.toTypedArray())
    }

    fun getExternalContentDirs(): Array<String> = directoryPreferences.getExternalContentDirs()

    fun setExternalContentDirs(dirs: Array<String>) = directoryPreferences.setExternalContentDirs(dirs)

    fun getSaveDir(): String = unsupported("Raitchi save locations are managed per supported adapter")
    fun getDefaultSaveDir(): String = unsupported("Raitchi save locations are managed per supported adapter")
    fun setSaveDir(path: String): Nothing = unsupported("Raitchi save locations are managed per supported adapter")
    fun getNandDir(): String = unsupported("Raitchi does not expose a generic NAND directory")
    fun setNandDir(path: String): Nothing = unsupported("Raitchi does not expose a generic NAND directory")
    fun getSdmcDir(): String = unsupported("Raitchi does not expose a generic SDMC directory")
    fun setSdmcDir(path: String): Nothing = unsupported("Raitchi does not expose a generic SDMC directory")

    private fun settingScope(needsGlobal: Boolean): String? = if (needsGlobal) null else activeAdapterId

    private fun checkedEngineOption(key: String, adapterId: String?): SettingsStore.Option =
        RaitchiShellBridge.settingOptions(adapterId).firstOrNull { it.key == key }
            ?: unsupported("Unsupported Raitchi setting: $key")

    /** Engine setters commit synchronously; validate the active scope before activity teardown. */
    private fun validateEngineScope(adapterId: String?) {
        RaitchiShellBridge.settingOptions(adapterId).forEach { option ->
            RaitchiShellBridge.getSetting(appContext, adapterId, option.key)
        }
    }

    private fun flushAppPreferences() {
        val preferences = appPreferences
        val nextRevision = preferences.getLong(PREF_SAVED_REVISION, 0) + 1
        checkCommit(preferences.edit().putLong(PREF_SAVED_REVISION, nextRevision).commit())
    }

    private fun parseBooleanSetting(value: String, key: String): Boolean = when (value) {
        "1", "true" -> true
        "0", "false" -> false
        else -> unsupported("Setting $key is not a boolean option")
    }

    private fun checkCommit(committed: Boolean) {
        check(committed) { "Could not persist Raitchi settings" }
    }

    private fun <T> unsupported(message: String): T = throw UnsupportedOperationException(message)
}
