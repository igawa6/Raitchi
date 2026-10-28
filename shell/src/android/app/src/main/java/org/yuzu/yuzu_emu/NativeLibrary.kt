// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu

import android.net.Uri
import androidx.annotation.Keep
import androidx.core.net.toUri
import com.lasrecomp.android.RaitchiMetadata
import com.lasrecomp.android.RaitchiShellBridge
import org.yuzu.yuzu_emu.utils.DocumentsTree
import org.yuzu.yuzu_emu.utils.FileUtil
import org.yuzu.yuzu_emu.utils.GameMetadata

/** Shell-only operations. SDL and the Raitchi engine own gameplay in :game. */
object NativeLibrary {
    @Keep
    @JvmStatic
    fun openContentUri(path: String?, openmode: String?): Int {
        return if (DocumentsTree.isNativePath(path!!)) {
            YuzuApplication.documentsTree!!.openContentUri(path, openmode)
        } else {
            FileUtil.openContentUri(path, openmode)
        }
    }

    @Keep
    @JvmStatic
    fun getSize(path: String?): Long {
        return if (DocumentsTree.isNativePath(path!!)) {
            YuzuApplication.documentsTree!!.getFileSize(path)
        } else {
            FileUtil.getFileSize(path)
        }
    }

    @Keep
    @JvmStatic
    fun exists(path: String?): Boolean {
        return if (DocumentsTree.isNativePath(path!!)) {
            YuzuApplication.documentsTree!!.exists(path)
        } else {
            FileUtil.exists(path, suppressLog = true)
        }
    }

    @Keep
    @JvmStatic
    fun isDirectory(path: String?): Boolean {
        return if (DocumentsTree.isNativePath(path!!)) {
            YuzuApplication.documentsTree!!.isDirectory(path)
        } else {
            FileUtil.isDirectory(path)
        }
    }

    @Keep
    @JvmStatic
    fun getParentDirectory(path: String): String =
        if (DocumentsTree.isNativePath(path)) {
            YuzuApplication.documentsTree!!.getParentDirectory(path)
        } else {
            path
        }

    @Keep
    @JvmStatic
    fun getFilename(path: String): String =
        if (DocumentsTree.isNativePath(path)) {
            YuzuApplication.documentsTree!!.getFilename(path)
        } else {
            FileUtil.getFilename(Uri.parse(path))
        }

    @Keep
    @JvmStatic
    fun copyFileToStorage(source: String, destdir: String): Boolean {
        return FileUtil.copyUriToInternalStorage(
            source.toUri(),
            destdir
        ) != null
    }

    @Keep
    @JvmStatic
    fun getFileExtension(source: String): String {
        return FileUtil.getExtension(source.toUri())
    }


    fun areKeysPresent(): Boolean = RaitchiMetadata.hasKeys(YuzuApplication.appContext)
    fun reloadKeys(): Boolean {
        GameMetadata.resetMetadata()
        return areKeysPresent()
    }
    fun isRunning(): Boolean = RaitchiShellBridge.gameRunning(YuzuApplication.appContext)
    fun getBuildVersion(): String = BuildConfig.VERSION_NAME
    fun getIcon(path: String): ByteArray = GameMetadata.getIcon(path)
}
