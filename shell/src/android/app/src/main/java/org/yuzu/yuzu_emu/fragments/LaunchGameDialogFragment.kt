// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.app.Dialog
import android.content.DialogInterface
import android.os.Bundle
import androidx.fragment.app.DialogFragment
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.SerializableHelper.parcelable

class LaunchGameDialogFragment : DialogFragment() {
    private var selectedItem = 1

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        val game = requireNotNull(requireArguments().parcelable<Game>(GAME))
        return MaterialAlertDialogBuilder(requireContext())
            .setTitle(game.title)
            .setMessage(when {
                game.compatible -> getString(R.string.start)
                game.tryable -> getString(R.string.game_try_message, game.compatibilityReason)
                else -> game.compatibilityReason
            })
            .setPositiveButton(if (game.tryable && !game.compatible) R.string.game_try_confirm else android.R.string.ok) { _, _ ->
                if (game.launchable) startActivity(game.launchIntent)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .create()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putInt(SELECTED_ITEM, selectedItem)
    }

    companion object {
        const val TAG = "LaunchGameDialogFragment"

        const val GAME = "Game"
        const val SELECTED_ITEM = "SelectedItem"

        fun newInstance(game: Game): LaunchGameDialogFragment {
            val args = Bundle()
            args.putParcelable(GAME, game)
            val fragment = LaunchGameDialogFragment()
            fragment.arguments = args
            return fragment
        }
    }
}
