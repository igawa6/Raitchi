// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.os.Bundle
import androidx.fragment.app.Fragment
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.fragment.app.activityViewModels
import androidx.recyclerview.widget.LinearLayoutManager
import com.fasterxml.jackson.databind.JsonNode
import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.transition.MaterialSharedAxis
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.FragmentDriverFetcherBinding
import org.yuzu.yuzu_emu.features.fetcher.DriverGroupAdapter
import org.yuzu.yuzu_emu.model.DriverViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.utils.ViewUtils.updateMargins
import java.io.IOException
import java.net.URL
import java.time.Instant
import java.time.LocalDateTime
import java.time.ZoneId
import kotlin.getValue

class DriverFetcherFragment : Fragment() {
    private var _binding: FragmentDriverFetcherBinding? = null
    private val binding get() = _binding!!

    private val client = OkHttpClient()

    enum class SortMode {
        Default, PublishTime,
    }

    private data class DriverRepo(
        val name: String = "",
        val path: String = "",
        val sort: Int = 0,
        val useTagName: Boolean = false,
        val sortMode: SortMode = SortMode.Default
    )

    private val repoList: List<DriverRepo> = listOf(
        DriverRepo("Mr. Purple Turnip", "MrPurple666/purple-turnip", 0),
        DriverRepo("GameHub Adreno 8xx", "crueter/GameHub-8Elite-Drivers", 1),
        DriverRepo("KIMCHI Turnip", "K11MCH1/AdrenoToolsDrivers", 2, true, SortMode.PublishTime),
        DriverRepo("Weab-Chan Freedreno", "Weab-chan/freedreno_turnip-CI", 3),
        DriverRepo("Whitebelyash Turnip", "whitebelyash/freedreno_turnip-CI", sort=4, false, SortMode.PublishTime),
    )

    private lateinit var driverGroupAdapter: DriverGroupAdapter
    private val driverViewModel: DriverViewModel by activityViewModels()
    private val homeViewModel: HomeViewModel by activityViewModels()

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
        _binding = FragmentDriverFetcherBinding.inflate(inflater)
        binding.badgeRecommendedDriver.text = getString(R.string.raitchi_driver_packages)
        binding.badgeGpuModel.visibility = View.GONE

        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(visible = false)
        binding.toolbarDrivers.setNavigationOnClickListener {
            requireActivity().onBackPressedDispatcher.onBackPressed()
        }

        binding.listDrivers.layoutManager = LinearLayoutManager(context)
        driverGroupAdapter = DriverGroupAdapter(requireActivity(), driverViewModel)
        binding.listDrivers.adapter = driverGroupAdapter

        setInsets()

        fetchDrivers()
    }

    private fun fetchDrivers() {
        binding.loadingIndicator.isVisible = true

        val driverGroups = arrayListOf<DriverGroup>()

        repoList.forEach { driver ->
            val name = driver.name
            val path = driver.path
            val useTagName = driver.useTagName
            val sortMode = driver.sortMode
            val sort = driver.sort

            viewLifecycleOwner.lifecycleScope.launch {
                val request =
                    Request.Builder().url("https://api.github.com/repos/$path/releases").build()

                withContext(Dispatchers.IO) {
                    var releases: ArrayList<Release>
                    try {
                        client.newCall(request).execute().use { response ->
                            if (!response.isSuccessful) {
                                throw IOException(response.body.toString())
                            }

                            val body = response.body?.string() ?: return@withContext
                            releases = Release.fromJsonArray(body, useTagName, sortMode)
                        }
                    } catch (cancelled: kotlinx.coroutines.CancellationException) {
                        throw cancelled
                    } catch (e: Exception) {
                        withContext(Dispatchers.Main) {
                            MaterialAlertDialogBuilder(requireActivity()).setTitle(
                                getString(R.string.error_during_fetch)
                            )
                                .setMessage(
                                    "${getString(R.string.failed_to_fetch)} $name:\n${e.message}"
                                )
                                .setPositiveButton(getString(R.string.ok)) { dialog, _ -> dialog.cancel() }
                                .show()

                            releases = ArrayList()
                        }
                    }

                    val group = DriverGroup(
                        name,
                        releases,
                        sort
                    )

                    synchronized(driverGroups) {
                        driverGroups.add(group)
                        driverGroups.sortBy {
                            it.sort
                        }
                    }

                    withContext(Dispatchers.Main) {
                        driverGroupAdapter.updateDriverGroups(driverGroups)

                        if (driverGroups.size >= repoList.size) {
                            binding.loadingIndicator.isVisible = false
                        }
                    }
                }
            }
        }
    }

    private fun setInsets() = ViewCompat.setOnApplyWindowInsetsListener(
        binding.root
    ) { _: View, windowInsets: WindowInsetsCompat ->
        val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
        val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())

        val leftInsets = barInsets.left + cutoutInsets.left
        val rightInsets = barInsets.right + cutoutInsets.right

        binding.toolbarDrivers.updateMargins(left = leftInsets, right = rightInsets)
        binding.listDrivers.updateMargins(left = leftInsets, right = rightInsets)

        binding.listDrivers.updatePadding(
            bottom = barInsets.bottom + resources.getDimensionPixelSize(
                R.dimen.spacing_bottom_list_fab
            )
        )

        windowInsets
    }

    data class Artifact(val url: URL, val name: String)

    data class Release(
        var tagName: String = "",
        var titleName: String = "",
        var title: String = "",
        var body: String = "",
        var artifacts: List<Artifact> = ArrayList(),
        var prerelease: Boolean = false,
        var latest: Boolean = false,
        var publishTime: LocalDateTime = LocalDateTime.now()
    ) {
        companion object {
            fun fromJsonArray(
                jsonString: String,
                useTagName: Boolean,
                sortMode: SortMode
            ): ArrayList<Release> {
                val mapper = jacksonObjectMapper()

                try {
                    val rootNode = mapper.readTree(jsonString)

                    val releases = ArrayList<Release>()

                    var latestRelease: Release? = null

                    if (rootNode.isArray) {
                        rootNode.forEach { node ->
                            val release = fromJson(node, useTagName)

                            if (latestRelease == null && !release.prerelease) {
                                latestRelease = release
                                release.latest = true
                            }

                            releases.add(release)
                        }
                    }

                    when (sortMode) {
                        SortMode.PublishTime -> releases.sortByDescending {
                            it.publishTime
                        }

                        else -> {}
                    }

                    return releases
                } catch (e: Exception) {
                    e.printStackTrace()
                    return ArrayList()
                }
            }

            private fun fromJson(node: JsonNode, useTagName: Boolean): Release {
                try {
                    val tagName = node.get("tag_name").toString().removeSurrounding("\"")
                    val body = node.get("body").toString().removeSurrounding("\"")
                    val prerelease = node.get("prerelease").toString().toBoolean()
                    val titleName = node.get("name").toString().removeSurrounding("\"")

                    val published = node.get("published_at").toString().removeSurrounding("\"")
                    val instantTime: Instant? = Instant.parse(published)
                    val localTime = instantTime?.atZone(ZoneId.systemDefault())?.toLocalDateTime() ?: LocalDateTime.now()

                    val title = if (useTagName) tagName else titleName

                    val assets = node.get("assets")
                    val artifacts = ArrayList<Artifact>()
                    if (assets?.isArray == true) {
                        assets.forEach { subNode ->
                            val urlStr = subNode.get("browser_download_url").toString()
                                .removeSurrounding("\"")

                            val url = URL(urlStr)
                            val name = subNode.get("name").toString().removeSurrounding("\"")

                            val artifact = Artifact(url, name)
                            artifacts.add(artifact)
                        }
                    }

                    return Release(
                        tagName,
                        titleName,
                        title,
                        body,
                        artifacts,
                        prerelease,
                        false,
                        localTime
                    )
                } catch (e: Exception) {
                    // TODO: handle malformed input.
                    e.printStackTrace()
                }

                return Release()
            }
        }
    }

    data class DriverGroup(
        val name: String,
        val releases: ArrayList<Release>,
        val sort: Int
    )
}
