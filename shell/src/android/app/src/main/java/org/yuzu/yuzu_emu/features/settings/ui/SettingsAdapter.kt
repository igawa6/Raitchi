// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.features.settings.ui

import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import androidx.lifecycle.ViewModelProvider
import androidx.recyclerview.widget.AsyncDifferConfig
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import org.yuzu.yuzu_emu.features.settings.model.BooleanSetting
import org.yuzu.yuzu_emu.databinding.ListItemSettingBinding
import org.yuzu.yuzu_emu.databinding.ListItemSettingSwitchBinding
import org.yuzu.yuzu_emu.databinding.ListItemSettingsHeaderBinding
import org.yuzu.yuzu_emu.features.settings.model.view.HeaderSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.features.settings.model.view.StringSingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SubmenuSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SwitchSetting
import org.yuzu.yuzu_emu.features.settings.ui.viewholder.HeaderViewHolder
import org.yuzu.yuzu_emu.features.settings.ui.viewholder.SettingViewHolder
import org.yuzu.yuzu_emu.features.settings.ui.viewholder.SingleChoiceViewHolder
import org.yuzu.yuzu_emu.features.settings.ui.viewholder.SubmenuViewHolder
import org.yuzu.yuzu_emu.features.settings.ui.viewholder.SwitchSettingViewHolder

/** Eden's existing RecyclerView adapter, restricted to supported shell setting rows. */
class SettingsAdapter(private val fragment: Fragment) :
    ListAdapter<SettingsItem, SettingViewHolder>(AsyncDifferConfig.Builder(DiffCallback()).build()) {
    private val settingsViewModel: SettingsViewModel
        get() = ViewModelProvider(fragment.requireActivity())[SettingsViewModel::class.java]

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): SettingViewHolder {
        val inflater = LayoutInflater.from(parent.context)
        return when (viewType) {
            SettingsItem.TYPE_HEADER -> HeaderViewHolder(
                ListItemSettingsHeaderBinding.inflate(inflater), this
            )
            SettingsItem.TYPE_SWITCH -> SwitchSettingViewHolder(
                ListItemSettingSwitchBinding.inflate(inflater), this
            )
            SettingsItem.TYPE_STRING_SINGLE_CHOICE -> SingleChoiceViewHolder(
                ListItemSettingBinding.inflate(inflater), this
            )
            SettingsItem.TYPE_SUBMENU -> SubmenuViewHolder(
                ListItemSettingBinding.inflate(inflater), this
            )
            else -> throw IllegalArgumentException("Unsupported Raitchi setting row type: $viewType")
        }
    }

    override fun onBindViewHolder(holder: SettingViewHolder, position: Int) {
        holder.bind(currentList[position])
    }

    override fun getItemViewType(position: Int): Int = currentList[position].type

    fun onBooleanClick(item: SwitchSetting, checked: Boolean, position: Int) {
        item.setChecked(checked)
        notifyItemChanged(position)
        settingsViewModel.setShouldReloadSettingsList(true)
        if (item.setting.key == BooleanSetting.BLACK_BACKGROUNDS.key) {
            fragment.requireActivity().recreate()
        }
    }

    fun onStringSingleChoiceClick(item: StringSingleChoiceSetting, position: Int) {
        SettingsDialogFragment.newInstance(
            settingsViewModel, item, SettingsItem.TYPE_STRING_SINGLE_CHOICE, position
        ).show(fragment.childFragmentManager, SettingsDialogFragment.TAG)
    }

    fun onSubmenuClick(item: SubmenuSetting) {
        throw UnsupportedOperationException("Unsupported settings screen: ${item.menuKey}")
    }

    fun onLongClick(item: SettingsItem, position: Int): Boolean {
        SettingsDialogFragment.newInstance(
            settingsViewModel, item, SettingsDialogFragment.TYPE_RESET_SETTING, position
        ).show(fragment.childFragmentManager, SettingsDialogFragment.TAG)
        return true
    }

    fun onClearClick(item: SettingsItem, position: Int) {
        item.setting.global = true
        notifyItemChanged(position)
        settingsViewModel.setShouldReloadSettingsList(true)
    }

    private class DiffCallback : DiffUtil.ItemCallback<SettingsItem>() {
        override fun areItemsTheSame(oldItem: SettingsItem, newItem: SettingsItem): Boolean =
            oldItem.setting.key == newItem.setting.key

        override fun areContentsTheSame(oldItem: SettingsItem, newItem: SettingsItem): Boolean =
            oldItem.setting.key == newItem.setting.key
    }
}
