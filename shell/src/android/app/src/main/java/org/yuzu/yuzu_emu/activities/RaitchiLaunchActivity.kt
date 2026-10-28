// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.activities

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.lasrecomp.android.RaitchiShellBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.DialogProgressBinding
import org.yuzu.yuzu_emu.utils.ThemeHelper

/** Prepares a scanned package, then hands the verified session to SDL in :game. */
class RaitchiLaunchActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        super.onCreate(savedInstanceState)
        val binding = DialogProgressBinding.inflate(layoutInflater)
        binding.title.setText(R.string.raitchi_preparing_game)
        binding.status.setText(R.string.raitchi_keep_open)
        setContentView(binding.root)
        lifecycleScope.launch {
            try {
                val source = intent.data ?: error("No game file selected")
                val adapter = intent.getStringExtra("adapterId") ?: error("No compatible adapter")
                val preparing = withContext(Dispatchers.IO) {
                    RaitchiShellBridge.prepareScannedGame(applicationContext, source, adapter)
                }
                while (RaitchiShellBridge.isPackageImportRunning()) {
                    binding.status.text = RaitchiShellBridge.packageImportStatus()
                    delay(200)
                }
                if (preparing && RaitchiShellBridge.packageImportStatus().startsWith("Preparation stopped:")) {
                    error(RaitchiShellBridge.packageImportStatus())
                }
                val game = withContext(Dispatchers.IO) {
                    RaitchiShellBridge.prepareSession(applicationContext, adapter)
                }
                startActivity(game)
                finish()
            } catch (cancelled: kotlinx.coroutines.CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                MaterialAlertDialogBuilder(this@RaitchiLaunchActivity)
                    .setTitle(R.string.raitchi_cannot_start)
                    .setMessage(failure.message)
                    .setPositiveButton(android.R.string.ok) { _, _ -> finish() }
                    .setOnCancelListener { finish() }.show()
            } catch (failure: LinkageError) {
                MaterialAlertDialogBuilder(this@RaitchiLaunchActivity)
                    .setTitle(R.string.raitchi_cannot_start)
                    .setMessage(failure.message)
                    .setPositiveButton(android.R.string.ok) { _, _ -> finish() }.show()
            }
        }
    }
}
