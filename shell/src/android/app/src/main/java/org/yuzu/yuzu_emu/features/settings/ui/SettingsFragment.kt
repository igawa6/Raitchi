// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.features.settings.ui

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.navigation.fragment.navArgs
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.databinding.FragmentSettingsBinding
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.utils.ViewUtils.updateMargins
import org.yuzu.yuzu_emu.utils.collect

/** Eden's retained toolbar/list settings screen backed by Raitchi's supported options. */
class SettingsFragment : Fragment() {
    private var _binding: FragmentSettingsBinding? = null
    private val binding get() = _binding!!
    private val args by navArgs<SettingsFragmentArgs>()
    private val settingsViewModel: SettingsViewModel by activityViewModels()
    private var settingsAdapter: SettingsAdapter? = null
    private lateinit var presenter: SettingsFragmentPresenter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        exitTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, state: Bundle?): View {
        _binding = FragmentSettingsBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        settingsAdapter = SettingsAdapter(this)
        presenter = SettingsFragmentPresenter(settingsViewModel, settingsAdapter!!, args.menuTag)
        binding.toolbarSettings.title = if (args.menuTag == Settings.MenuTag.SECTION_ROOT && args.game != null) {
            args.game!!.title
        } else {
            getString(args.menuTag.titleId)
        }
        binding.toolbarSettings.setNavigationOnClickListener {
            settingsViewModel.setShouldNavigateBack(true)
        }
        binding.listSettings.apply {
            adapter = settingsAdapter
            layoutManager = LinearLayoutManager(requireContext())
        }
        settingsViewModel.shouldReloadSettingsList.collect(viewLifecycleOwner,
            resetState = { settingsViewModel.setShouldReloadSettingsList(false) }) {
            if (it) presenter.loadSettingsList()
        }
        settingsViewModel.adapterItemChanged.collect(viewLifecycleOwner,
            resetState = { settingsViewModel.setAdapterItemChanged(-1) }) {
            if (it != -1) settingsAdapter?.notifyItemChanged(it)
        }
        settingsViewModel.datasetChanged.collect(viewLifecycleOwner,
            resetState = { settingsViewModel.setDatasetChanged(false) }) {
            if (it) presenter.loadSettingsList()
        }
        presenter.onViewCreated()
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutout = insets.getInsets(WindowInsetsCompat.Type.displayCutout())
            binding.listSettings.updateMargins(left = bars.left + cutout.left, right = bars.right + cutout.right)
            binding.listSettings.updatePadding(bottom = bars.bottom)
            binding.appbarSettings.updateMargins(left = bars.left + cutout.left, right = bars.right + cutout.right)
            insets
        }
    }

    override fun onDestroyView() {
        settingsAdapter = null
        _binding = null
        super.onDestroyView()
    }
}
