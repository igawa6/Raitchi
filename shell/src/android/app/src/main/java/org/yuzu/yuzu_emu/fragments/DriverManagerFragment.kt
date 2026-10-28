// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.navigation.findNavController
import androidx.recyclerview.widget.GridLayoutManager
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.HomeNavigationDirections
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.adapters.DriverAdapter
import org.yuzu.yuzu_emu.databinding.FragmentDriverManagerBinding
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen
import org.yuzu.yuzu_emu.model.DriverViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.utils.FileUtil
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.utils.ViewUtils.updateMargins
import org.yuzu.yuzu_emu.utils.collect
import java.io.File
import java.io.IOException

class DriverManagerFragment : Fragment() {
    private var _binding: FragmentDriverManagerBinding? = null
    private val binding get() = _binding!!

    private val homeViewModel: HomeViewModel by activityViewModels()
    private val driverViewModel: DriverViewModel by activityViewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentDriverManagerBinding.inflate(inflater)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(visible = false)

        driverViewModel.onOpenDriverManager()
        driverViewModel.driverError.collect(viewLifecycleOwner) { error ->
            if (error != null) {
                Toast.makeText(requireContext(), error, Toast.LENGTH_LONG).show()
                driverViewModel.consumeDriverError()
            }
        }
        driverViewModel.driverStatus.collect(viewLifecycleOwner) { status ->
            binding.toolbarDrivers.subtitle = status.takeIf(String::isNotBlank)
        }

        if (!driverViewModel.isInteractionAllowed.value) {
            DriversLoadingDialogFragment().show(
                childFragmentManager,
                DriversLoadingDialogFragment.TAG
            )
        }

        binding.toolbarDrivers.setNavigationOnClickListener {
            requireActivity().onBackPressedDispatcher.onBackPressed()
        }

        binding.buttonInstall.setOnClickListener {
            getDriver.launch(arrayOf("application/zip"))
        }

        binding.buttonFetch.setOnClickListener {
            val action = HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(
                SettingsSubscreen.DRIVER_FETCHER,
                null
            )
            binding.root.findNavController().navigate(action)
        }

        binding.listDrivers.apply {
            layoutManager = GridLayoutManager(
                requireContext(),
                resources.getInteger(R.integer.grid_columns)
            )
            adapter = DriverAdapter(driverViewModel)
        }
        driverViewModel.driverList.collect(viewLifecycleOwner) { drivers ->
            (binding.listDrivers.adapter as? DriverAdapter)?.replaceList(drivers)
        }

        setInsets()

    }

    override fun onDestroy() {
        super.onDestroy()
        driverViewModel.onCloseDriverManager()
    }

    override fun onResume() {
        super.onResume()
        refreshDriverList()
    }

    private fun refreshDriverList() {
        driverViewModel.reloadDriverData()
        (binding.listDrivers.adapter as? DriverAdapter)
            ?.replaceList(driverViewModel.driverList.value)
    }

    private fun setInsets() =
        ViewCompat.setOnApplyWindowInsetsListener(
            binding.root
        ) { _: View, windowInsets: WindowInsetsCompat ->
            val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())

            val leftInsets = barInsets.left + cutoutInsets.left
            val rightInsets = barInsets.right + cutoutInsets.right

            binding.toolbarDrivers.updateMargins(left = leftInsets, right = rightInsets)
            binding.listDrivers.updateMargins(left = leftInsets, right = rightInsets)

            val fabSpacing = resources.getDimensionPixelSize(R.dimen.spacing_fab)
            binding.buttonInstall.updateMargins(
                left = leftInsets + fabSpacing,
                right = rightInsets + fabSpacing,
                bottom = barInsets.bottom + fabSpacing
            )

            binding.buttonFetch.updateMargins(
                left = leftInsets + fabSpacing,
                right = rightInsets + fabSpacing,
                bottom = barInsets.bottom + fabSpacing
            )

            binding.listDrivers.updatePadding(
                bottom = barInsets.bottom +
                    resources.getDimensionPixelSize(R.dimen.spacing_bottom_list_fab)
            )

            windowInsets
        }

    private val getDriver =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { result ->
            if (result == null) {
                return@registerForActivityResult
            }

            ProgressDialogFragment.newInstance(
                requireActivity(),
                R.string.installing_driver,
                false
            ) { _, _ ->
                val driverFile = File(
                    GpuDriverHelper.driverStoragePath,
                    FileUtil.getFilename(result)
                )
                try {
                    val existingMetadata = withContext(Dispatchers.IO) {
                        GpuDriverHelper.getMetadataFromZip(driverFile)
                    }
                    if (existingMetadata.name != null) {
                        return@newInstance getString(R.string.driver_already_installed)
                    }
                    withContext(Dispatchers.IO) {
                        if (driverFile.exists()) driverFile.delete()
                        if (!GpuDriverHelper.copyDriverToInternalStorage(result)) {
                            throw IOException("Driver failed validation")
                        }
                    }
                } catch (error: IOException) {
                    return@newInstance error.message ?: getString(R.string.select_gpu_driver_error)
                }

                val driverData = GpuDriverHelper.getMetadataFromZip(driverFile)
                val duplicate = driverViewModel.driverData.any { it.second == driverData }
                if (duplicate) {
                    withContext(Dispatchers.IO) { driverFile.delete() }
                    return@newInstance getString(R.string.driver_already_installed)
                } else {
                    val installStatus = try {
                        driverViewModel.installAddedDriver(
                            Pair(driverFile.absolutePath, driverData)
                        )
                    } catch (error: Exception) {
                        return@newInstance error.message
                            ?: getString(R.string.select_gpu_driver_error)
                    }
                    if (!driverViewModel.isDriverActive(driverData)) {
                        return@newInstance installStatus.ifBlank {
                            getString(R.string.select_gpu_driver_error)
                        }
                    }
                    withContext(Dispatchers.Main) {
                        if (_binding != null) {
                            refreshDriverList()
                            val adapter = binding.listDrivers.adapter as DriverAdapter
                            val selectedPosition = adapter.currentList
                                .indexOfFirst { it.selected }
                                .let { if (it == -1) 0 else it }
                            binding.listDrivers
                                .smoothScrollToPosition(selectedPosition)
                        }
                    }
                }
                return@newInstance Any()
            }.show(childFragmentManager, ProgressDialogFragment.TAG)
        }

}
