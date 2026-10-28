// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.features.settings.ui

import com.lasrecomp.android.RaitchiShellBridge
import org.yuzu.yuzu_emu.features.settings.model.AbstractStringSetting
import org.yuzu.yuzu_emu.features.settings.model.BooleanSetting
import org.yuzu.yuzu_emu.features.settings.model.RaitchiStringSetting
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.features.settings.model.view.HeaderSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.features.settings.model.view.StringSingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SwitchSetting
import org.yuzu.yuzu_emu.utils.NativeConfig

/** Populates Eden's existing setting rows from the active Raitchi settings adapter. */
class SettingsFragmentPresenter(
    private val settingsViewModel: SettingsViewModel,
    private val adapter: SettingsAdapter,
    private val menuTag: Settings.MenuTag
) {
    fun onViewCreated() = loadSettingsList()

    fun loadSettingsList() {
        val game = settingsViewModel.game
        val rows = ArrayList<SettingsItem>()
        when (menuTag) {
            Settings.MenuTag.SECTION_ROOT -> {
                val options = RaitchiShellBridge.settingOptions(game?.adapterId)
                var category: String? = null
                for (option in options) {
                    if (category != option.category) {
                        category = option.category
                        rows.add(HeaderSetting(titleString = option.category))
                    }
                    if (option.key == "render_scale" && game != null &&
                        !RaitchiShellBridge.supportsRenderScaling(game.adapterId)
                    ) {
                        rows.add(
                            StringSingleChoiceSetting(
                                stringSetting = NativeResolutionNoticeSetting,
                                titleString = option.label,
                                descriptionString = "Resolution scaling is not supported for this game yet.",
                                choices = arrayOf("Native size"),
                                values = arrayOf("Native size")
                            )
                        )
                        continue
                    }
                    rows.add(
                        StringSingleChoiceSetting(
                            stringSetting = RaitchiStringSetting(option.key),
                            titleString = option.label,
                            descriptionString = if (game == null && option.key == "render_scale")
                                "Applies to supported games; other games use native size."
                            else "",
                            choices = option.labels,
                            values = option.values
                        )
                    )
                }
            }
            Settings.MenuTag.SECTION_APP_SETTINGS -> {
                val resources = org.yuzu.yuzu_emu.YuzuApplication.appContext.resources
                rows.add(HeaderSetting(titleString = "Appearance"))
                rows.add(
                    StringSingleChoiceSetting(
                        stringSetting = ThemeModeSetting,
                        titleId = org.yuzu.yuzu_emu.R.string.change_theme_mode,
                        choices = resources.getStringArray(org.yuzu.yuzu_emu.R.array.themeModeEntries),
                        values = resources.getIntArray(org.yuzu.yuzu_emu.R.array.themeModeValues)
                            .map { it.toString() }.toTypedArray()
                    )
                )
                rows.add(SwitchSetting(BooleanSetting.BLACK_BACKGROUNDS,
                    titleId = org.yuzu.yuzu_emu.R.string.use_black_backgrounds,
                    descriptionId = org.yuzu.yuzu_emu.R.string.use_black_backgrounds_description))
            }
            else -> Unit
        }
        adapter.submitList(rows)
    }
}

private object NativeResolutionNoticeSetting : AbstractStringSetting {
    override val key = "render_scale"
    override val defaultValue = "Native size"
    override val isRuntimeModifiable = false
    override val pairedSettingKey = ""
    override val isSwitchable = false
    override val isSaveable = false
    override var global: Boolean
        get() = true
        set(value) {}

    override fun getString(needsGlobal: Boolean) = "Native size"
    override fun setString(value: String) = Unit
    override fun getValueAsString(needsGlobal: Boolean) = "Native size"
    override fun reset() = Unit
}

private object ThemeModeSetting : AbstractStringSetting {
    override val key = "theme_mode"
    override val defaultValue: String get() = NativeConfig.getDefaultToString(key)
    override val isRuntimeModifiable = true
    override val pairedSettingKey = ""
    override val isSwitchable = false
    override val isSaveable = true
    override var global: Boolean
        get() = NativeConfig.usingGlobal(key)
        set(value) = NativeConfig.setGlobal(key, value)

    override fun getString(needsGlobal: Boolean): String = NativeConfig.getInt(key, needsGlobal).toString()
    override fun setString(value: String) {
        val mode = value.toIntOrNull() ?: throw IllegalArgumentException("Invalid theme mode")
        require(mode == -1 || mode == 1 || mode == 2) { "Invalid theme mode" }
        NativeConfig.setInt(key, mode)
    }
    override fun getValueAsString(needsGlobal: Boolean): String = getString(needsGlobal)
    override fun reset() = setString(defaultValue)
}
