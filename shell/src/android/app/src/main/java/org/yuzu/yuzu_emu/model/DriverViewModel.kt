// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.model

import android.content.Context
import android.net.Uri
import androidx.core.content.edit
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lasrecomp.android.RaitchiShellBridge
import java.io.File
import java.io.IOException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.model.Driver.Companion.toDriver
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.utils.GpuDriverMetadata

class DriverViewModel : ViewModel() {
    private val appContext = YuzuApplication.appContext
    private val driverPreferences = appContext.getSharedPreferences(
        PREFS_NAME,
        Context.MODE_PRIVATE
    )
    private val driverOperationMutex = Mutex()

    private val _areDriversLoading = MutableStateFlow(false)
    private val _isDriverReady = MutableStateFlow(true)
    private val _isDeletingDrivers = MutableStateFlow(false)

    val isInteractionAllowed: StateFlow<Boolean> = combine(
        _areDriversLoading,
        _isDriverReady,
        _isDeletingDrivers
    ) { loading, ready, deleting -> !loading && ready && !deleting }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(), initialValue = false)

    var driverData: MutableList<Pair<String, GpuDriverMetadata>> = loadDrivers()
        private set

    private val _driverList = MutableStateFlow(emptyList<Driver>())
    val driverList: StateFlow<List<Driver>> get() = _driverList

    private val _selectedDriverTitle = MutableStateFlow("")
    val selectedDriverTitle: StateFlow<String> get() = _selectedDriverTitle

    private val _selectedDriverVersion = MutableStateFlow("")
    val selectedDriverVersion: StateFlow<String> get() = _selectedDriverVersion

    private val _driverStatus = MutableStateFlow("")
    val driverStatus: StateFlow<String> get() = _driverStatus.asStateFlow()

    private val _driverError = MutableStateFlow<String?>(null)
    val driverError: StateFlow<String?> get() = _driverError.asStateFlow()

    private val driversToDelete = mutableListOf<String>()
    private var pendingOperations = 0

    init {
        updateDriverList()
        updateDriverNameForGame(null)
    }

    private fun loadDrivers(): MutableList<Pair<String, GpuDriverMetadata>> =
        runCatching { GpuDriverHelper.getDrivers() }.getOrDefault(mutableListOf())

    fun reloadDriverData() {
        _areDriversLoading.value = true
        driverData = loadDrivers().filterNot { it.first in driversToDelete }.toMutableList()
        updateDriverList()
        _areDriversLoading.value = pendingOperations > 0
    }

    fun updateDriverList() {
        val selectedPath = driverPreferences.getString(KEY_SELECTED_DRIVER_PATH, null)
        val driverEnabled = runCatching { RaitchiShellBridge.isDriverEnabled(appContext) }
            .getOrDefault(false)
        _driverStatus.value = runCatching { RaitchiShellBridge.driverStatus(appContext) }
            .getOrDefault("")
        val selectedDriver = if (driverEnabled) {
            driverData.firstOrNull { it.first == selectedPath }?.second
        } else {
            null
        }
        val systemDriverTitle = appContext.getString(R.string.system_gpu_driver)
        val items = mutableListOf(
            Driver(
                selected = selectedDriver == null,
                title = systemDriverTitle,
                version = systemDriverTitle,
                description = ""
            )
        )
        driverData.forEach { (_, metadata) ->
            items.add(metadata.toDriver(metadata == selectedDriver))
        }
        _driverList.value = items
        updateName(selectedDriver)
    }

    fun onOpenDriverManager() {
        reloadDriverData()
    }

    fun onDriverSelected(position: Int) {
        val path = if (position == 0) null else driverData.getOrNull(position - 1)?.first
        if (position > 0 && path == null) {
            updateDriverList()
            return
        }
        val activePath = driverPreferences.getString(KEY_SELECTED_DRIVER_PATH, null)
        val enabled = runCatching { RaitchiShellBridge.isDriverEnabled(appContext) }
            .getOrDefault(false)
        if ((path == null && !enabled) || (path != null && enabled && path == activePath)) {
            updateDriverList()
            return
        }
        scheduleDriverSelection(path)
    }

    private fun scheduleDriverSelection(path: String?) {
        viewModelScope.launch(Dispatchers.Main.immediate) {
            try {
                selectDriverAndWait(path)
            } catch (_: Exception) {
                // The error is exposed through driverError for the manager UI.
            }
        }
    }

    private suspend fun selectDriverAndWait(path: String?): String {
        if (path != null && isDriverActiveByName(File(path).name)) {
            updateDriverList()
            return driverData.firstOrNull { it.first == path }?.second?.name
                ?.takeIf(String::isNotBlank)
                ?: throw IOException("Selected driver metadata is unavailable")
        }
        pendingOperations++
        _areDriversLoading.value = true
        try {
            return driverOperationMutex.withLock {
                withContext(Dispatchers.IO) {
                    if (path == null) {
                        RaitchiShellBridge.setDriverEnabled(appContext, false)
                        driverPreferences.edit().remove(KEY_SELECTED_DRIVER_PATH).apply()
                        appContext.getString(R.string.system_gpu_driver)
                    } else {
                        val archive = File(path)
                        if (!archive.isFile) {
                            throw IOException("Driver ZIP is missing")
                        }
                        val status = RaitchiShellBridge.installDriver(appContext, Uri.fromFile(archive))
                        driverPreferences.edit().putString(KEY_SELECTED_DRIVER_PATH, path).apply()
                        status
                    }
                }
            }
        } catch (error: Exception) {
            _driverError.value = error.message ?: appContext.getString(R.string.fatal_error)
            if (path == null) {
                driverPreferences.getString(KEY_SELECTED_DRIVER_PATH, null)?.let { activePath ->
                    driversToDelete.remove(activePath)
                    val activeArchive = File(activePath)
                    val metadata = GpuDriverHelper.getMetadataFromZip(activeArchive)
                    if (metadata.name != null && driverData.none { it.first == activePath }) {
                        driverData.add(activePath to metadata)
                    }
                }
            }
            throw error
        } finally {
            pendingOperations--
            _areDriversLoading.value = pendingOperations > 0
            updateDriverList()
        }
    }

    fun consumeDriverError() {
        _driverError.value = null
    }

    fun isDriverActiveByName(fileName: String): Boolean {
        val selectedPath = driverPreferences.getString(KEY_SELECTED_DRIVER_PATH, null) ?: return false
        val isEnabled = runCatching { RaitchiShellBridge.isDriverEnabled(appContext) }
            .getOrDefault(false)
        return isEnabled && File(selectedPath).name.equals(fileName, ignoreCase = true)
    }

    fun isDriverActive(metadata: GpuDriverMetadata): Boolean {
        val selectedPath = driverPreferences.getString(KEY_SELECTED_DRIVER_PATH, null) ?: return false
        val isEnabled = runCatching { RaitchiShellBridge.isDriverEnabled(appContext) }
            .getOrDefault(false)
        return isEnabled && driverData.firstOrNull { it.first == selectedPath }?.second == metadata
    }

    fun onDriverRemoved(removedPosition: Int, @Suppress("UNUSED_PARAMETER") selectedPosition: Int) {
        val driverIndex = removedPosition - 1
        if (driverIndex !in driverData.indices) {
            updateDriverList()
            return
        }
        val removedPath = driverData.removeAt(driverIndex).first
        driversToDelete.add(removedPath)
        val selectedPath = driverPreferences.getString(KEY_SELECTED_DRIVER_PATH, null)
        if (selectedPath == removedPath) {
            scheduleDriverSelection(null)
        }
        updateDriverList()
    }

    fun onDriverAdded(driver: Pair<String, GpuDriverMetadata>) {
        viewModelScope.launch(Dispatchers.Main.immediate) {
            try {
                installAddedDriver(driver)
            } catch (_: Exception) {
                // The error is exposed through driverError for the manager UI.
            }
        }
    }

    suspend fun installAddedDriver(driver: Pair<String, GpuDriverMetadata>): String =
        withContext(Dispatchers.Main.immediate) {
            driversToDelete.remove(driver.first)
            val existingIndex = driverData.indexOfFirst {
                it.first == driver.first || it.second == driver.second
            }
            val selectedPath = if (existingIndex != -1) {
                val existingPath = driverData[existingIndex].first
                if (existingPath != driver.first) {
                    withContext(Dispatchers.IO) { File(driver.first).delete() }
                }
                existingPath
            } else {
                driverData.add(driver)
                driver.first
            }
            updateDriverList()
            selectDriverAndWait(selectedPath)
        }

    fun onCloseDriverManager() {
        val pendingDeletes = driversToDelete.distinct()
        if (pendingDeletes.isEmpty()) {
            reloadDriverData()
            return
        }

        _isDeletingDrivers.value = true
        viewModelScope.launch {
            try {
                driverOperationMutex.withLock {
                    val activePath = if (RaitchiShellBridge.isDriverEnabled(appContext)) {
                        driverPreferences.getString(KEY_SELECTED_DRIVER_PATH, null)
                    } else {
                        null
                    }
                    val preserveActive = activePath?.takeIf { it in pendingDeletes }
                    val deleteCandidates = pendingDeletes.filterNot { it == preserveActive }
                    val failedDeletes = withContext(Dispatchers.IO) {
                        deleteCandidates.filter { path ->
                            val file = File(path)
                            file.exists() && !file.delete()
                        }
                    }
                    driversToDelete.removeAll(pendingDeletes.toSet())
                    if (failedDeletes.isNotEmpty()) {
                        _driverError.value = "Could not remove ${failedDeletes.size} driver archive(s)"
                    }
                }
            } catch (error: Exception) {
                driversToDelete.removeAll(pendingDeletes.toSet())
                _driverError.value = error.message ?: appContext.getString(R.string.fatal_error)
            } finally {
                _isDeletingDrivers.value = false
                reloadDriverData()
            }
        }
    }

    /** The engine driver state is process-independent and is configured from the library UI. */
    fun onLaunchGame() {
        _isDriverReady.value = true
    }

    fun updateDriverNameForGame(@Suppress("UNUSED_PARAMETER") game: Game?) {
        updateDriverList()
    }

    private fun updateName(selectedDriver: GpuDriverMetadata?) {
        val systemDriverTitle = appContext.getString(R.string.system_gpu_driver)
        _selectedDriverTitle.value = selectedDriver?.name ?: systemDriverTitle
        _selectedDriverVersion.value = selectedDriver?.version ?: systemDriverTitle
    }

    companion object {
        private const val PREFS_NAME = "raitchi_driver_preferences"
        private const val KEY_SELECTED_DRIVER_PATH = "selected_driver_zip"
    }
}
