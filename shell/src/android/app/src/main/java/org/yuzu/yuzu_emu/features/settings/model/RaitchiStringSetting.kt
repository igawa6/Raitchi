// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.features.settings.model

import org.yuzu.yuzu_emu.utils.NativeConfig

/** Eden setting-model adapter for a validated Raitchi string option. */
class RaitchiStringSetting(override val key: String) : AbstractStringSetting {
    override val defaultValue: String
        get() = NativeConfig.getDefaultToString(key)

    override val isRuntimeModifiable: Boolean = false
    override val pairedSettingKey: String = ""
    override val isSwitchable: Boolean = true
    override val isSaveable: Boolean = true

    override var global: Boolean
        get() = NativeConfig.usingGlobal(key)
        set(value) = NativeConfig.setGlobal(key, value)

    override fun getString(needsGlobal: Boolean): String = NativeConfig.getString(key, needsGlobal)

    override fun setString(value: String) {
        if (NativeConfig.isPerGameConfigLoaded()) global = false
        NativeConfig.setString(key, value)
    }

    override fun getValueAsString(needsGlobal: Boolean): String = getString(needsGlobal)

    override fun reset() = setString(defaultValue)
}
