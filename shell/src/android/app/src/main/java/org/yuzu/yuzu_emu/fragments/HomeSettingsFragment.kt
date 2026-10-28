// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.navigation.findNavController
import androidx.navigation.fragment.findNavController
import androidx.recyclerview.widget.GridLayoutManager
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.adapters.HomeSettingAdapter
import org.yuzu.yuzu_emu.databinding.FragmentHomeSettingsBinding
import org.yuzu.yuzu_emu.features.fetcher.SpacingItemDecoration
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen
import org.yuzu.yuzu_emu.model.DriverViewModel
import org.yuzu.yuzu_emu.model.HomeSetting
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.ui.main.MainActivity
import org.yuzu.yuzu_emu.utils.ViewUtils.updateMargins

class HomeSettingsFragment : Fragment() {
    private var _binding: FragmentHomeSettingsBinding? = null
    private val binding get() = _binding!!

    private lateinit var mainActivity: MainActivity

    private val homeViewModel: HomeViewModel by activityViewModels()
    private val driverViewModel: DriverViewModel by activityViewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentHomeSettingsBinding.inflate(layoutInflater)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(visible = false)
        mainActivity = requireActivity() as MainActivity
        binding.toolbarHomeSettings.setNavigationOnClickListener {
            findNavController().popBackStack()
        }
        binding.toolbarHomeSettings.title = getString(R.string.preferences_settings)

        val optionsList = mutableListOf(
            HomeSetting(R.string.advanced_settings, R.string.settings_description, R.drawable.ic_settings, {
                binding.root.findNavController().navigate(HomeNavigationDirections.actionGlobalSettingsActivity(
                    null, Settings.MenuTag.SECTION_ROOT))
            }),
            HomeSetting(R.string.app_settings, R.string.app_settings_description, R.drawable.ic_palette, {
                binding.root.findNavController().navigate(HomeNavigationDirections.actionGlobalSettingsActivity(
                    null, Settings.MenuTag.SECTION_APP_SETTINGS))
            }),
            HomeSetting(R.string.gpu_driver_manager, R.string.install_gpu_driver_description, R.drawable.ic_build, {
                binding.root.findNavController().navigate(HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(
                    SettingsSubscreen.DRIVER_MANAGER, null))
            }, details = driverViewModel.selectedDriverTitle),
            HomeSetting(R.string.game_folders, R.string.raitchi_game_folders_description, R.drawable.ic_folder_open, {
                binding.root.findNavController().navigate(HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(
                    SettingsSubscreen.GAME_FOLDERS, null))
            }),
            HomeSetting(R.string.keys, R.string.keys_description, R.drawable.ic_key, {
                mainActivity.getProdKey.launch(arrayOf("*/*"))
            }),
            HomeSetting(R.string.about, R.string.about_description, R.drawable.ic_info_outline, {
                binding.root.findNavController().navigate(HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(
                    SettingsSubscreen.ABOUT, null))
            })
        )

        binding.homeSettingsList.apply {
            layoutManager =
                GridLayoutManager(requireContext(), resources.getInteger(R.integer.grid_columns))
            adapter = HomeSettingAdapter(
                requireActivity() as AppCompatActivity,
                viewLifecycleOwner,
                optionsList
            )
            val spacing = resources.getDimensionPixelSize(R.dimen.spacing_small)
            addItemDecoration(SpacingItemDecoration(spacing))
        }

        setInsets()
    }

    override fun onStart() {
        super.onStart()
        exitTransition = null
    }

    override fun onResume() {
        super.onResume()
        driverViewModel.updateDriverNameForGame(null)
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun setInsets() =
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, windowInsets ->
            val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())

            binding.appbarHomeSettings.updateMargins(
                left = barInsets.left + cutoutInsets.left,
                right = barInsets.right + cutoutInsets.right
            )

            binding.scrollViewSettings.updatePadding(
                bottom = barInsets.bottom
            )

            binding.homeSettingsList.updatePadding(
                left = barInsets.left + cutoutInsets.left,
                right = barInsets.right + cutoutInsets.right
            )

            windowInsets
        }
}
