// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.features.settings.model.view

import androidx.annotation.StringRes
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.features.settings.model.AbstractSetting
import org.yuzu.yuzu_emu.utils.NativeConfig

/** Common data and edit-state rules for Eden's retained RecyclerView setting rows. */
abstract class SettingsItem(
    val setting: AbstractSetting,
    @StringRes val titleId: Int,
    val titleString: String,
    @StringRes val descriptionId: Int,
    val descriptionString: String
) {
    abstract val type: Int

    val title: String by lazy {
        if (titleId != 0) YuzuApplication.appContext.getString(titleId) else titleString
    }
    val description: String by lazy {
        if (descriptionId != 0) YuzuApplication.appContext.getString(descriptionId) else descriptionString
    }

    val isEditable: Boolean
        get() {
            if (NativeConfig.isPerGameConfigLoaded() && !setting.isSaveable) return false
            if (NativeLibrary.isRunning()) {
                if (!NativeConfig.isPerGameConfigLoaded() && !setting.global) return false
                return setting.isRuntimeModifiable
            }
            return true
        }

    val needsRuntimeGlobal: Boolean
        get() = NativeLibrary.isRunning() && !setting.global && !NativeConfig.isPerGameConfigLoaded()

    val clearable: Boolean
        get() = !setting.global && NativeConfig.isPerGameConfigLoaded()

    companion object {
        const val TYPE_HEADER = 0
        const val TYPE_SWITCH = 1
        const val TYPE_SINGLE_CHOICE = 2
        const val TYPE_SLIDER = 3
        const val TYPE_SUBMENU = 4
        const val TYPE_STRING_SINGLE_CHOICE = 5
        const val TYPE_DATETIME_SETTING = 6
        const val TYPE_RUNNABLE = 7
        const val TYPE_INPUT = 8
        const val TYPE_INT_SINGLE_CHOICE = 9
        const val TYPE_INPUT_PROFILE = 10
        const val TYPE_STRING_INPUT = 11
        const val TYPE_SPINBOX = 12
        const val TYPE_LAUNCHABLE = 13
        const val TYPE_PATH = 14
        const val TYPE_GPU_UNSWIZZLE = 15
        const val TYPE_FX_TOOLBAR = 16
        const val TYPE_FX_PRESET = 17
        const val TYPE_FX_SHADER = 18
        const val TYPE_FX_BUTTON = 19

        const val FASTMEM_COMBINED = "fastmem_combined"
        const val GPU_UNSWIZZLE_COMBINED = "gpu_unswizzle_combined"

        val emptySetting = object : AbstractSetting {
            override val key: String = ""
            override val defaultValue: Any = false
            override val isRuntimeModifiable: Boolean = false
            override val pairedSettingKey: String = ""
            override val isSwitchable: Boolean = false
            override var global: Boolean = true
            override val isSaveable: Boolean = true
            override fun getValueAsString(needsGlobal: Boolean): String = ""
            override fun reset() = Unit
        }
    }
}
