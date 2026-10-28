// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.adapters

import com.lasrecomp.android.RaitchiShellBridge
import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.navigation.findNavController
import androidx.preference.PreferenceManager
import androidx.viewbinding.ViewBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.databinding.CardGameListBinding
import org.yuzu.yuzu_emu.databinding.CardGameGridBinding
import org.yuzu.yuzu_emu.databinding.CardGameCarouselBinding
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.GamesViewModel
import org.yuzu.yuzu_emu.utils.GameIconUtils
import org.yuzu.yuzu_emu.utils.UntestedGame
import org.yuzu.yuzu_emu.utils.ViewUtils.marquee
import org.yuzu.yuzu_emu.viewholder.AbstractViewHolder
import androidx.core.net.toUri
import androidx.core.content.edit
import org.yuzu.yuzu_emu.databinding.CardGameGridCompactBinding

class GameAdapter(private val activity: AppCompatActivity) :
    AbstractDiffAdapter<Game, GameAdapter.GameViewHolder>(exact = false) {

    companion object {
        const val VIEW_TYPE_GRID = 0
        const val VIEW_TYPE_GRID_COMPACT = 1
        const val VIEW_TYPE_LIST = 2
        const val VIEW_TYPE_CAROUSEL = 3
    }

    private var viewType = 0

    fun setViewType(type: Int) {
        viewType = type
        notifyDataSetChanged()
    }

    public var cardSize: Int = 0
        private set

    fun setCardSize(size: Int) {
        if (cardSize != size && size > 0) {
            cardSize = size
            notifyDataSetChanged()
        }
    }

    override fun getItemViewType(position: Int): Int = viewType

    override fun onBindViewHolder(holder: GameViewHolder, position: Int) {
        super.onBindViewHolder(holder, position)
        when (getItemViewType(position)) {
            VIEW_TYPE_LIST -> {
                val listBinding = holder.binding as CardGameListBinding
                listBinding.cardGameList.scaleX = 1f
                listBinding.cardGameList.scaleY = 1f
                listBinding.cardGameList.alpha = 1f
                // Reset layout params to XML defaults
                listBinding.root.layoutParams.width = ViewGroup.LayoutParams.MATCH_PARENT
                listBinding.root.layoutParams.height = ViewGroup.LayoutParams.WRAP_CONTENT
            }

            VIEW_TYPE_GRID -> {
                val gridBinding = holder.binding as CardGameGridBinding
                gridBinding.cardGameGrid.scaleX = 1f
                gridBinding.cardGameGrid.scaleY = 1f
                gridBinding.cardGameGrid.alpha = 1f
                // Reset layout params to XML defaults
                gridBinding.root.layoutParams.width = ViewGroup.LayoutParams.MATCH_PARENT
                gridBinding.root.layoutParams.height = ViewGroup.LayoutParams.WRAP_CONTENT
            }

            VIEW_TYPE_GRID_COMPACT -> {
                val gridCompactBinding = holder.binding as CardGameGridCompactBinding
                gridCompactBinding.cardGameGridCompact.scaleX = 1f
                gridCompactBinding.cardGameGridCompact.scaleY = 1f
                gridCompactBinding.cardGameGridCompact.alpha = 1f
                // Reset layout params to XML defaults (same as normal grid)
                gridCompactBinding.root.layoutParams.width = ViewGroup.LayoutParams.MATCH_PARENT
                gridCompactBinding.root.layoutParams.height = ViewGroup.LayoutParams.WRAP_CONTENT
            }

            VIEW_TYPE_CAROUSEL -> {
                val carouselBinding = holder.binding as CardGameCarouselBinding
                // soothens transient flickering
                carouselBinding.cardGameCarousel.scaleY = 0f
                carouselBinding.cardGameCarousel.alpha = 0f
            }
        }
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): GameViewHolder {
        val binding = when (viewType) {
            VIEW_TYPE_LIST -> CardGameListBinding.inflate(
                LayoutInflater.from(parent.context),
                parent,
                false
            )

            VIEW_TYPE_GRID -> CardGameGridBinding.inflate(
                LayoutInflater.from(parent.context),
                parent,
                false
            )

            VIEW_TYPE_GRID_COMPACT -> CardGameGridCompactBinding.inflate(
                LayoutInflater.from(parent.context),
                parent,
                false
            )

            VIEW_TYPE_CAROUSEL -> CardGameCarouselBinding.inflate(
                LayoutInflater.from(parent.context),
                parent,
                false
            )

            else -> throw IllegalArgumentException("Invalid view type")
        }
        return GameViewHolder(binding, viewType)
    }

    inner class GameViewHolder(
        internal val binding: ViewBinding,
        private val viewType: Int
    ) : AbstractViewHolder<Game>(binding) {

        override fun bind(model: Game) {
            when (viewType) {
                VIEW_TYPE_LIST -> bindListView(model)
                VIEW_TYPE_GRID -> bindGridView(model)
                VIEW_TYPE_CAROUSEL -> bindCarouselView(model)
                VIEW_TYPE_GRID_COMPACT -> bindGridCompactView(model)
            }
        }

        private fun bindListView(model: Game) {
            val listBinding = binding as CardGameListBinding

            listBinding.imageGameScreen.scaleType = ImageView.ScaleType.CENTER_CROP
            GameIconUtils.loadGameIcon(model, listBinding.imageGameScreen)

            listBinding.textGameTitle.text = model.title.replace("[\\t\\n\\r]+".toRegex(), " ")
            listBinding.textGameDeveloper.text = model.developer
            showCompatibility(listBinding.textGameCompatibility, model)

            listBinding.textGameTitle.marquee()
            listBinding.cardGameList.setOnClickListener { onClick(model) }
            listBinding.cardGameList.setOnLongClickListener { onLongClick(model) }

            // Reset layout params to XML defaults
            listBinding.root.layoutParams.width = ViewGroup.LayoutParams.MATCH_PARENT
            listBinding.root.layoutParams.height = ViewGroup.LayoutParams.WRAP_CONTENT
        }

        private fun bindGridView(model: Game) {
            val gridBinding = binding as CardGameGridBinding

            gridBinding.imageGameScreen.scaleType = ImageView.ScaleType.CENTER_CROP
            GameIconUtils.loadGameIcon(model, gridBinding.imageGameScreen)

            gridBinding.textGameTitle.text = model.title.replace("[\\t\\n\\r]+".toRegex(), " ")
            showCompatibility(gridBinding.textGameCompatibility, model)

            gridBinding.textGameTitle.marquee()
            gridBinding.cardGameGrid.setOnClickListener { onClick(model) }
            gridBinding.cardGameGrid.setOnLongClickListener { onLongClick(model) }

            // Reset layout params to XML defaults
            gridBinding.root.layoutParams.width = ViewGroup.LayoutParams.MATCH_PARENT
            gridBinding.root.layoutParams.height = ViewGroup.LayoutParams.WRAP_CONTENT
        }

        private fun bindGridCompactView(model: Game) {
            val gridCompactBinding = binding as CardGameGridCompactBinding

            gridCompactBinding.imageGameScreenCompact.scaleType = ImageView.ScaleType.CENTER_CROP
            GameIconUtils.loadGameIcon(model, gridCompactBinding.imageGameScreenCompact)

            gridCompactBinding.textGameTitleCompact.text = model.title.replace("[\\t\\n\\r]+".toRegex(), " ")
            showCompatibility(gridCompactBinding.textGameCompatibility, model)

            gridCompactBinding.textGameTitleCompact.marquee()
            gridCompactBinding.cardGameGridCompact.setOnClickListener { onClick(model) }
            gridCompactBinding.cardGameGridCompact.setOnLongClickListener { onLongClick(model) }

            // Reset layout params to XML defaults (same as normal grid)
            gridCompactBinding.root.layoutParams.width = ViewGroup.LayoutParams.MATCH_PARENT
            gridCompactBinding.root.layoutParams.height = ViewGroup.LayoutParams.WRAP_CONTENT
        }

        private fun bindCarouselView(model: Game) {
            val carouselBinding = binding as CardGameCarouselBinding

            carouselBinding.imageGameScreen.scaleType = ImageView.ScaleType.CENTER_CROP
            GameIconUtils.loadGameIcon(model, carouselBinding.imageGameScreen)

            carouselBinding.textGameTitle.text = model.title.replace("[\\t\\n\\r]+".toRegex(), " ")
            carouselBinding.textGameCompatibility?.let { showCompatibility(it, model) }
            carouselBinding.textGameTitle.marquee()
            carouselBinding.cardGameCarousel.setOnClickListener { onClick(model) }
            carouselBinding.cardGameCarousel.setOnLongClickListener { onLongClick(model) }

            carouselBinding.imageGameScreen.contentDescription =
                binding.root.context.getString(R.string.game_image_desc, model.title)

            // Ensure zero-heighted-full-width cards for carousel
            carouselBinding.root.layoutParams.width = cardSize
        }

        fun onClick(game: Game) {
            if (!game.compatible) {
                UntestedGame.confirmLaunch(activity, game) { start(game) }
                return
            }
            start(game)
        }

        private fun start(game: Game) {
            val gameExists = DocumentFile.fromSingleUri(
                YuzuApplication.appContext,
                game.path.toUri()
            )?.exists() == true

            if (!gameExists) {
                Toast.makeText(
                    YuzuApplication.appContext,
                    R.string.loader_error_file_not_found,
                    Toast.LENGTH_LONG
                ).show()

                ViewModelProvider(activity)[GamesViewModel::class.java].reloadGames(true)
                return
            }

            val launch: () -> Unit = {
                val preferences =
                    PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext)
                preferences.edit {
                    putLong(
                        game.keyLastPlayedTime,
                        System.currentTimeMillis()
                    )
                }

                activity.lifecycleScope.launch {
                    withContext(Dispatchers.IO) {
                        val shortcut =
                            ShortcutInfoCompat.Builder(YuzuApplication.appContext, game.path)
                                .setShortLabel(game.title)
                                .setIcon(GameIconUtils.getShortcutIcon(activity, game))
                                .setIntent(game.launchIntent)
                                .build()
                        ShortcutManagerCompat.pushDynamicShortcut(
                            YuzuApplication.appContext,
                            shortcut
                        )
                    }
                }

                activity.startActivity(game.launchIntent)
            }
            launch()
        }

        private fun showCompatibility(view: android.widget.TextView, game: Game) {
            val adapter = game.adapterId.ifBlank { activity.getString(R.string.adapter_unknown) }
            view.text = activity.getString(
                if (game.compatible && RaitchiShellBridge.isAdapterExperimental(game.adapterId)) R.string.game_adapter_experimental
                else if (game.compatible) R.string.game_adapter_supported
                else if (game.tryable) R.string.game_adapter_untested else R.string.game_adapter_unsupported,
                adapter
            )
            view.contentDescription = if (game.compatible) view.text else
                "${view.text}: ${game.compatibilityReason}"
            view.setTextColor(activity.getColor(
                if (game.launchable) R.color.yuzu_primary_pink else R.color.yuzu_primary_red
            ))
        }

        fun onLongClick(game: Game): Boolean {
            val action = HomeNavigationDirections.actionGlobalPerGamePropertiesFragment(game)
            binding.root.findNavController().navigate(action)
            return true
        }
    }
}
