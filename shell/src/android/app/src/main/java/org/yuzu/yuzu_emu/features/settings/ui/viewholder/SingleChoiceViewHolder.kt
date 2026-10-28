// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.features.settings.ui.viewholder

import android.view.View
import org.yuzu.yuzu_emu.databinding.ListItemSettingBinding
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.features.settings.model.view.StringSingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.ui.SettingsAdapter
import org.yuzu.yuzu_emu.utils.ViewUtils.setVisible

class SingleChoiceViewHolder(private val binding: ListItemSettingBinding, adapter: SettingsAdapter) :
    SettingViewHolder(binding.root, adapter) {
    private val name = binding.textSettingName
    private val description = binding.textSettingDescription
    private val value = binding.textSettingValue
    private val clear = binding.buttonClear
    private lateinit var setting: StringSingleChoiceSetting

    override fun bind(item: SettingsItem) {
        setting = item as? StringSingleChoiceSetting
            ?: throw IllegalArgumentException("Expected a Raitchi string choice row")
        name.text = setting.title
        description.setVisible(setting.description.isNotEmpty())
        description.text = setting.description
        value.text = setting.getSelectedDisplayText(setting.needsRuntimeGlobal)
        value.setVisible(value.text.isNotEmpty())
        clear.setVisible(setting.clearable)
        clear.setOnClickListener { adapter.onClearClick(setting, bindingAdapterPosition) }
        setStyle(setting.isEditable, binding)
    }

    override fun onClick(clicked: View) {
        if (setting.isEditable) adapter.onStringSingleChoiceClick(setting, bindingAdapterPosition)
    }

    override fun onLongClick(clicked: View): Boolean =
        setting.isEditable && adapter.onLongClick(setting, bindingAdapterPosition)
}
