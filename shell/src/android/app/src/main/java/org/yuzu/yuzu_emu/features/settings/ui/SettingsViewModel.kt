// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.features.settings.ui

import androidx.lifecycle.ViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.model.Game

/** State used by Eden's retained settings activity, list, and option dialogs. */
class SettingsViewModel : ViewModel() {
    var game: Game? = null
    var clickedItem: SettingsItem? = null

    val shouldRecreate: StateFlow<Boolean> get() = _shouldRecreate
    private val _shouldRecreate = MutableStateFlow(false)
    val shouldNavigateBack: StateFlow<Boolean> get() = _shouldNavigateBack
    private val _shouldNavigateBack = MutableStateFlow(false)
    val shouldShowResetSettingsDialog: StateFlow<Boolean> get() = _shouldShowResetSettingsDialog
    private val _shouldShowResetSettingsDialog = MutableStateFlow(false)
    val shouldReloadSettingsList: StateFlow<Boolean> get() = _shouldReloadSettingsList
    private val _shouldReloadSettingsList = MutableStateFlow(false)
    val adapterItemChanged: StateFlow<Int> get() = _adapterItemChanged
    private val _adapterItemChanged = MutableStateFlow(-1)
    private val _datasetChanged = MutableStateFlow(false)
    val datasetChanged = _datasetChanged.asStateFlow()
    val shouldRecreateForLanguageChange: StateFlow<Boolean> get() = _shouldRecreateForLanguageChange
    private val _shouldRecreateForLanguageChange = MutableStateFlow(false)

    fun setShouldRecreate(value: Boolean) { _shouldRecreate.value = value }
    fun setShouldNavigateBack(value: Boolean) { _shouldNavigateBack.value = value }
    fun setShouldShowResetSettingsDialog(value: Boolean) { _shouldShowResetSettingsDialog.value = value }
    fun setShouldReloadSettingsList(value: Boolean) { _shouldReloadSettingsList.value = value }
    fun setAdapterItemChanged(value: Int) { _adapterItemChanged.value = value }
    fun setDatasetChanged(value: Boolean) { _datasetChanged.value = value }
    fun setShouldRecreateForLanguageChange(value: Boolean) {
        _shouldRecreateForLanguageChange.value = value
    }
}
