// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.Context
import android.widget.Toast
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Game

/** Gate for launching: supported games start, untested games ask first, others explain why not. */
object UntestedGame {
    fun confirmLaunch(context: Context, game: Game, launch: () -> Unit) {
        when {
            game.compatible -> launch()
            game.tryable -> MaterialAlertDialogBuilder(context)
                .setTitle(R.string.game_try_title)
                .setMessage(context.getString(R.string.game_try_message, game.compatibilityReason))
                .setPositiveButton(R.string.game_try_confirm) { _, _ -> launch() }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
            else -> Toast.makeText(
                context,
                context.getString(R.string.game_unsupported_reason, game.compatibilityReason),
                Toast.LENGTH_LONG
            ).show()
        }
    }
}
