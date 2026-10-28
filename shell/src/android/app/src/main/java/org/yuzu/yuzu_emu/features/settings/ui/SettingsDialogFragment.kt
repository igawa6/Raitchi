// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.features.settings.ui

import android.app.Dialog
import android.content.DialogInterface
import android.os.Bundle
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.activityViewModels
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.features.settings.model.view.StringSingleChoiceSetting

/** Choice/reset dialogs for the retained Raitchi settings rows. */
class SettingsDialogFragment : DialogFragment(), DialogInterface.OnClickListener {
    private val settingsViewModel: SettingsViewModel by activityViewModels()
    private var type: Int = 0
    private var position: Int = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        type = requireArguments().getInt(TYPE)
        position = requireArguments().getInt(POSITION)
    }

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        val item = requireNotNull(settingsViewModel.clickedItem) { "Setting row is no longer available" }
        return when (type) {
            TYPE_RESET_SETTING -> MaterialAlertDialogBuilder(requireContext())
                .setMessage(R.string.reset_setting_confirmation)
                .setPositiveButton(android.R.string.ok) { _, _ ->
                    item.setting.reset()
                    settingsViewModel.setShouldReloadSettingsList(true)
                    finishDialog()
                }
                .setNegativeButton(android.R.string.cancel) { _, _ -> finishDialog() }
                .setOnCancelListener { finishDialog() }
                .create()

            SettingsItem.TYPE_STRING_SINGLE_CHOICE -> {
                val choice = item as? StringSingleChoiceSetting
                    ?: throw IllegalArgumentException("Expected a string choice setting")
                MaterialAlertDialogBuilder(requireContext())
                    .setTitle(choice.title)
                    .setSingleChoiceItems(choice.choices, choice.selectedValueIndex, this)
                    .create()
            }

            else -> throw UnsupportedOperationException("Unsupported Raitchi setting dialog type: $type")
        }
    }

    override fun onClick(dialog: DialogInterface, which: Int) {
        val choice = settingsViewModel.clickedItem as? StringSingleChoiceSetting
            ?: throw IllegalStateException("No string choice is open")
        choice.setSelectedValue(choice.getValueAt(which))
        settingsViewModel.setShouldReloadSettingsList(true)
        if (choice.setting.key == "theme_mode") requireActivity().recreate()
        finishDialog()
    }

    private fun finishDialog() {
        settingsViewModel.setAdapterItemChanged(position)
        settingsViewModel.clickedItem = null
        dismiss()
    }

    companion object {
        const val TAG = "SettingsDialogFragment"
        const val TYPE_RESET_SETTING = -1
        private const val TYPE = "Type"
        private const val POSITION = "Position"

        fun newInstance(
            settingsViewModel: SettingsViewModel,
            clickedItem: SettingsItem,
            type: Int,
            position: Int
        ): SettingsDialogFragment {
            settingsViewModel.clickedItem = clickedItem
            return SettingsDialogFragment().apply {
                arguments = Bundle().apply {
                    putInt(TYPE, type)
                    putInt(POSITION, position)
                }
            }
        }
    }
}
