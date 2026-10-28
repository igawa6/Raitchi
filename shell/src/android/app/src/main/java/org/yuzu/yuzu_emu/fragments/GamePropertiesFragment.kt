// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.os.Bundle
import com.lasrecomp.android.RaitchiShellBridge
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.lifecycleScope
import androidx.navigation.findNavController
import androidx.navigation.fragment.navArgs
import androidx.recyclerview.widget.StaggeredGridLayoutManager
import com.google.android.material.transition.MaterialSharedAxis
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.adapters.GamePropertiesAdapter
import org.yuzu.yuzu_emu.databinding.FragmentGamePropertiesBinding
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen
import org.yuzu.yuzu_emu.model.GameProperty
import org.yuzu.yuzu_emu.model.GamesViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.model.SubmenuProperty
import org.yuzu.yuzu_emu.utils.GameIconUtils
import org.yuzu.yuzu_emu.utils.ViewUtils.marquee
import org.yuzu.yuzu_emu.utils.ViewUtils.updateMargins
import org.yuzu.yuzu_emu.utils.collect

class GamePropertiesFragment : Fragment() {
    private var _binding: FragmentGamePropertiesBinding? = null
    private val binding get() = _binding!!

    private val homeViewModel: HomeViewModel by activityViewModels()
    private val gamesViewModel: GamesViewModel by activityViewModels()

    private val args by navArgs<GamePropertiesFragmentArgs>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.Y, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.Y, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentGamePropertiesBinding.inflate(layoutInflater)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(true)

        binding.buttonBack.setOnClickListener {
            view.findNavController().popBackStack()
        }

        val shortcutManager = requireActivity().getSystemService(ShortcutManager::class.java)
        binding.buttonShortcut.isEnabled =
            shortcutManager.isRequestPinShortcutSupported && args.game.compatible
        binding.buttonShortcut.setOnClickListener {
            if (!args.game.compatible) return@setOnClickListener
            viewLifecycleOwner.lifecycleScope.launch {
                withContext(Dispatchers.IO) {
                    val shortcut = ShortcutInfo.Builder(requireContext(), args.game.title)
                        .setShortLabel(args.game.title)
                        .setIcon(
                            GameIconUtils.getShortcutIcon(requireActivity(), args.game)
                                .toIcon(requireContext())
                        )
                        .setIntent(args.game.launchIntent)
                        .build()
                    shortcutManager.requestPinShortcut(shortcut, null)
                }
            }
        }

        GameIconUtils.loadGameIcon(args.game, binding.imageGameScreen)
        binding.title.text = args.game.title
        binding.title.marquee()

        val adapterName = args.game.adapterId.ifBlank { getString(R.string.adapter_unknown) }
        binding.compatibility?.text = getString(
            if (args.game.compatible && RaitchiShellBridge.isAdapterExperimental(args.game.adapterId)) R.string.game_adapter_experimental
            else if (args.game.compatible) R.string.game_adapter_supported
            else if (args.game.tryable) R.string.game_adapter_untested
            else R.string.game_adapter_unsupported,
            adapterName
        ) + if (args.game.compatible) "" else " · ${args.game.compatibilityReason}"
        binding.buttonStart.isEnabled = args.game.launchable
        binding.buttonStart.contentDescription = if (args.game.launchable) {
            getString(R.string.start)
        } else {
            getString(R.string.game_unsupported_reason, args.game.compatibilityReason)
        }

        binding.buttonStart.setOnClickListener {
            LaunchGameDialogFragment.newInstance(args.game)
                .show(childFragmentManager, LaunchGameDialogFragment.TAG)
        }

        reloadList()

        homeViewModel.reloadPropertiesList.collect(
            viewLifecycleOwner,
            resetState = { homeViewModel.reloadPropertiesList(false) }
        ) { if (it) reloadList() }

        setInsets()
    }

    override fun onDestroy() {
        val isChangingConfigurations = activity?.isChangingConfigurations == true
        super.onDestroy()
        if (!isChangingConfigurations) {
            gamesViewModel.reloadGames(true)
        }
    }

    private fun reloadList() {
        _binding ?: return

        val properties = mutableListOf<GameProperty>(
            SubmenuProperty(R.string.info, R.string.info_description, R.drawable.ic_info_outline,
                action = {
                    binding.root.findNavController().navigate(HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(
                        SettingsSubscreen.GAME_INFO, args.game))
                })
        )
        if (args.game.compatible) {
            properties.add(SubmenuProperty(R.string.preferences_settings,
                R.string.per_game_settings_description, R.drawable.ic_settings,
                action = {
                    binding.root.findNavController().navigate(HomeNavigationDirections.actionGlobalSettingsActivity(
                        args.game, Settings.MenuTag.SECTION_ROOT))
                }))
        }
        binding.listProperties.apply {
            val spanCount = resources.getInteger(R.integer.grid_columns)
            val staggered = StaggeredGridLayoutManager(
                spanCount,
                StaggeredGridLayoutManager.VERTICAL
            ).apply {
                gapStrategy = StaggeredGridLayoutManager.GAP_HANDLING_MOVE_ITEMS_BETWEEN_SPANS
            }
            layoutManager = staggered
            adapter = GamePropertiesAdapter(viewLifecycleOwner, properties)
        }
    }

    override fun onResume() {
        super.onResume()
        reloadList()
    }

    private fun setInsets() =
        ViewCompat.setOnApplyWindowInsetsListener(
            binding.root
        ) { _: View, windowInsets: WindowInsetsCompat ->
            val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())

            val leftInsets = barInsets.left + cutoutInsets.left
            val rightInsets = barInsets.right + cutoutInsets.right

            val smallLayout = resources.getBoolean(R.bool.small_layout)
            if (smallLayout) {
                binding.listAll.updateMargins(left = leftInsets, right = rightInsets)
            } else {
                if (ViewCompat.getLayoutDirection(binding.root) ==
                    ViewCompat.LAYOUT_DIRECTION_LTR
                ) {
                    binding.listAll.updateMargins(right = rightInsets)
                    binding.iconLayout!!.updateMargins(top = barInsets.top, left = leftInsets)
                } else {
                    binding.listAll.updateMargins(left = leftInsets)
                    binding.iconLayout!!.updateMargins(top = barInsets.top, right = rightInsets)
                }
            }

            val fabSpacing = resources.getDimensionPixelSize(R.dimen.spacing_fab)
            binding.buttonStart.updateMargins(
                left = leftInsets + fabSpacing,
                right = rightInsets + fabSpacing,
                bottom = barInsets.bottom + fabSpacing
            )

            binding.layoutAll.updatePadding(
                top = barInsets.top,
                bottom = barInsets.bottom +
                    resources.getDimensionPixelSize(R.dimen.spacing_bottom_list_fab)
            )

            windowInsets
        }

}
