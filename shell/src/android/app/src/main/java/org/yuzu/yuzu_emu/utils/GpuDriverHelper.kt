// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.utils

import android.net.Uri
import android.os.Build
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.util.zip.ZipException
import java.util.zip.ZipFile

/** ZIP storage and metadata for the Raitchi engine's optional GPU drivers. */
object GpuDriverHelper {
    private const val META_JSON_FILENAME = "meta.json"

    val driverStoragePath: String
        get() = File(DirectoryInitialization.userDirectory!!, "gpu_drivers").absolutePath + "/"

    fun getDrivers(): MutableList<Pair<String, GpuDriverMetadata>> =
        File(driverStoragePath).listFiles()
            ?.asSequence()
            ?.filter { it.isFile }
            ?.mapNotNull { file ->
                val metadata = getMetadataFromZip(file)
                metadata.name?.let { file.absolutePath to metadata }
            }
            ?.sortedByDescending { it.second.name }
            ?.distinct()
            ?.toMutableList()
            ?: mutableListOf()

    /** Copies a selected ZIP into shell-owned storage after checking its metadata and API level. */
    fun copyDriverToInternalStorage(driverUri: Uri): Boolean {
        File(driverStoragePath).mkdirs()
        val copiedFile = FileUtil.copyUriToInternalStorage(driverUri, driverStoragePath)
            ?: return false
        val metadata = getMetadataFromZip(copiedFile)
        if (metadata.name == null || metadata.minApi > Build.VERSION.SDK_INT) {
            copiedFile.delete()
            return false
        }
        return true
    }

    fun getMetadataFromZip(driver: File): GpuDriverMetadata {
        if (!driver.isFile) return GpuDriverMetadata()
        try {
            ZipFile(driver).use { zip ->
                val entries = zip.entries()
                while (entries.hasMoreElements()) {
                    val entry = entries.nextElement()
                    if (!entry.isDirectory && entry.name.lowercase().endsWith(".json")) {
                        zip.getInputStream(entry).use { stream ->
                            return GpuDriverMetadata(stream, entry.size)
                        }
                    }
                }
            }
        } catch (_: ZipException) {
        } catch (_: FileNotFoundException) {
        } catch (_: IOException) {
        }
        return GpuDriverMetadata()
    }

    fun isDriverZipInstalledByName(fileName: String): Boolean {
        val baseName = fileName.substringAfterLast('/').substringAfterLast('\\')
        val candidate = File(driverStoragePath, baseName)
        if (!candidate.isFile || candidate.length() == 0L) return false
        return getMetadataFromZip(candidate).name != null
    }
}
