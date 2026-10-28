// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu

import android.content.Intent
import android.os.Looper
import android.view.View
import androidx.navigation.fragment.NavHostFragment
import androidx.preference.PreferenceManager
import androidx.recyclerview.widget.RecyclerView
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.yuzu.yuzu_emu.features.settings.model.RaitchiStringSetting
import org.yuzu.yuzu_emu.features.settings.model.AbstractStringSetting
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.features.settings.model.view.StringSingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.ui.SettingsActivity
import org.yuzu.yuzu_emu.features.settings.ui.SettingsAdapter
import org.yuzu.yuzu_emu.features.settings.ui.SettingsFragment
import org.yuzu.yuzu_emu.features.settings.ui.viewholder.SingleChoiceViewHolder
import org.yuzu.yuzu_emu.databinding.ListItemSettingBinding
import org.yuzu.yuzu_emu.fragments.SetupFragment
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.GameDir
import org.yuzu.yuzu_emu.model.DirectoryType
import org.yuzu.yuzu_emu.ui.GamesFragment
import org.yuzu.yuzu_emu.ui.main.MainActivity
import org.yuzu.yuzu_emu.utils.NativeConfig
import java.util.concurrent.TimeUnit
import com.lasrecomp.android.RaitchiMetadata

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = YuzuApplication::class)
class RaitchiShellUiTest {
    @Test
    fun firstLaunchLibraryAndSettingsUseTheEdenShell() {
        val app = RuntimeEnvironment.getApplication() as YuzuApplication
        app.getSharedPreferences("raitchi_settings", 0).edit().clear().commit()
        app.getSharedPreferences("raitchi_shell_config", 0).edit().clear().commit()
        PreferenceManager.getDefaultSharedPreferences(app).edit().clear()
            .putBoolean(Settings.PREF_FIRST_APP_LAUNCH, true).commit()
        NativeConfig.reloadGlobalConfig()

        val main = Robolectric.buildActivity(MainActivity::class.java).setup()
        idleMainLooper()
        val mainNavHost = main.get().supportFragmentManager
            .findFragmentById(R.id.fragment_container) as NavHostFragment
        assertEquals(R.id.firstTimeSetupFragment, mainNavHost.navController.currentDestination?.id)
        assertTrue(mainNavHost.childFragmentManager.primaryNavigationFragment is SetupFragment)
        assertTrue(NativeConfig.getGameDirs().isEmpty())
        assertFalse(RaitchiMetadata.hasKeys(app))

        mainNavHost.navController.navigate(R.id.action_firstTimeSetupFragment_to_gamesFragment)
        idleMainLooper()
        assertEquals(R.id.gamesFragment, mainNavHost.navController.currentDestination?.id)
        val gamesFragment = mainNavHost.childFragmentManager.primaryNavigationFragment as GamesFragment
        awaitVisible(gamesFragment, R.id.notice_text)

        val globalSettings = launchSettings(app, game = null)
        val globalFragment = currentSettingsFragment(globalSettings)
        val globalRows = awaitRows(globalFragment)
        val globalChoices = globalRows.filterIsInstance<StringSingleChoiceSetting>()
        val globalRecycler = globalFragment.requireView().findViewById<RecyclerView>(R.id.list_settings)
        val globalAdapter = globalRecycler.adapter as SettingsAdapter
        for (choice in globalChoices) {
            assertEquals("Choice labels for ${choice.setting.key}", choice.values.size, choice.choices.size)
            val original = choice.getSelectedValue()
            assertTrue("Default value for ${choice.setting.key} must have a label", choice.selectedValueIndex >= 0)
            val binding = ListItemSettingBinding.inflate(globalSettings.get().layoutInflater)
            val holder = SingleChoiceViewHolder(binding, globalAdapter)
            holder.bind(choice)
            assertEquals(choice.getSelectedDisplayText(), binding.textSettingValue.text.toString())
            for (index in choice.values.indices) {
                choice.setSelectedValue(choice.values[index])
                assertEquals(choice.values[index], choice.getSelectedValue())
                assertEquals(choice.choices[index], choice.getSelectedDisplayText())
                holder.bind(choice)
                assertEquals(choice.choices[index], binding.textSettingValue.text.toString())
            }
            choice.setSelectedValue(original)
            assertEquals(original, choice.getSelectedValue())
            holder.bind(choice)
            assertEquals(choice.getSelectedDisplayText(), binding.textSettingValue.text.toString())
        }
        val renderScale = globalChoices.first { it.setting.key == "render_scale" }
        assertEquals(
            listOf("0.5× native", "0.667× native (recommended)", "1× native", "Dynamic"),
            renderScale.choices.toList()
        )
        val showFps = globalChoices.first { it.setting.key == "show_fps" }
        showFps.setSelectedValue("0")
        assertEquals("0", NativeConfig.getString("show_fps"))
        assertEquals("Off", showFps.getSelectedDisplayText())
        showFps.setSelectedValue("1")
        assertEquals("On", showFps.getSelectedDisplayText())

        val globalVolume = globalRows.filterIsInstance<StringSingleChoiceSetting>()
            .first { it.setting.key == "volume" }
        assertTrue(globalVolume.setting is RaitchiStringSetting)
        globalVolume.setSelectedValue("0.50")
        assertEquals("0.50", NativeConfig.getString("volume"))
        assertEquals(globalVolume.choices[globalVolume.values.indexOf("0.50")], globalVolume.getSelectedDisplayText())
        globalSettings.get().finish()
        idleMainLooper()

        val syntheticGame = Game(
            title = "Supported adapter smoke test",
            path = "content://raitchi.test/synthetic.nsp",
            programId = "0100000000000000",
            adapterId = "las",
            compatible = true
        )
        val titleSettings = launchSettings(app, syntheticGame)
        val titleRows = awaitRows(currentSettingsFragment(titleSettings))
        val titleVolume = titleRows.filterIsInstance<StringSingleChoiceSetting>()
            .first { it.setting.key == "volume" }
        assertTrue(titleVolume.setting.global)
        assertEquals("0.50", titleVolume.getSelectedValue())
        assertEquals(globalVolume.getSelectedDisplayText(), titleVolume.getSelectedDisplayText())
        val titleRecycler = currentSettingsFragment(titleSettings).requireView()
            .findViewById<RecyclerView>(R.id.list_settings)
        val titleBinding = ListItemSettingBinding.inflate(titleSettings.get().layoutInflater)
        val titleHolder = SingleChoiceViewHolder(titleBinding, titleRecycler.adapter as SettingsAdapter)
        titleHolder.bind(titleVolume)
        assertEquals(globalVolume.getSelectedDisplayText(), titleBinding.textSettingValue.text.toString())

        titleVolume.setSelectedValue("0.75")
        assertFalse(titleVolume.setting.global)
        assertEquals("0.75", NativeConfig.getString("volume"))
        assertEquals(titleVolume.choices[titleVolume.values.indexOf("0.75")], titleVolume.getSelectedDisplayText())
        titleHolder.bind(titleVolume)
        assertEquals(titleVolume.getSelectedDisplayText(), titleBinding.textSettingValue.text.toString())
        titleVolume.setting.global = true
        assertTrue(titleVolume.setting.global)
        assertEquals("0.50", titleVolume.getSelectedValue())

        titleSettings.get().finish()
        main.get().finish()
        idleMainLooper()
    }

    @Test
    fun stringChoiceSummaryUsesAnUnknownLabelForUnmappedValues() {
        var rawValue = "unexpected-enum"
        val setting = object : AbstractStringSetting {
            override val key = "test_choice"
            override val defaultValue: Any = "0"
            override val isRuntimeModifiable = false
            override val pairedSettingKey = ""
            override val isSwitchable = false
            override val isSaveable = false
            override var global = true
            override fun getString(needsGlobal: Boolean) = rawValue
            override fun setString(value: String) { rawValue = value }
            override fun getValueAsString(needsGlobal: Boolean) = rawValue
            override fun reset() { rawValue = defaultValue as String }
        }
        val choice = StringSingleChoiceSetting(
            setting,
            titleString = "Test choice",
            choices = arrayOf("Off", "On"),
            values = arrayOf("0", "1")
        )

        assertEquals("unexpected-enum", choice.getSelectedValue())
        assertEquals("Unknown value", choice.getSelectedDisplayText())
        assertEquals(-1, choice.selectedValueIndex)
    }

    @Test
    fun uiIntegerAndBooleanSettingsPersistTypedValues() {
        val app = RuntimeEnvironment.getApplication() as YuzuApplication
        val preferences = app.getSharedPreferences("raitchi_shell_config", 0)
        preferences.edit().clear().commit()
        NativeConfig.unloadPerGameConfig()

        NativeConfig.setInt("theme", 1)
        assertEquals(1, NativeConfig.getInt("theme"))
        assertEquals(1, preferences.getInt("theme", -1))
        NativeConfig.setString("app_language", "12")
        assertEquals(12, NativeConfig.getInt("app_language"))
        NativeConfig.setInt("control_opacity", 75)
        assertEquals(75, NativeConfig.getInt("control_opacity"))
        assertThrows(UnsupportedOperationException::class.java) {
            NativeConfig.setString("theme", "not an integer")
        }
        assertEquals(1, NativeConfig.getInt("theme"))

        for (value in listOf("1", "true", "TRUE")) {
            NativeConfig.setString("show_input_overlay", value)
            assertTrue(NativeConfig.getBoolean("show_input_overlay"))
        }
        for (value in listOf("0", "false", "FALSE")) {
            NativeConfig.setString("show_input_overlay", value)
            assertFalse(NativeConfig.getBoolean("show_input_overlay"))
        }
        assertThrows(UnsupportedOperationException::class.java) {
            NativeConfig.setString("show_input_overlay", "unexpected")
        }
        assertFalse(NativeConfig.getBoolean("show_input_overlay"))
    }

    @Test
    fun directoryPreferencesKeepValidEntriesWhenStoredDataIsDamaged() {
        val app = RuntimeEnvironment.getApplication() as YuzuApplication
        val preferences = app.getSharedPreferences("raitchi_shell_config", 0)
        preferences.edit().clear()
            .putString("game_dirs_v1", "not JSON")
            .putString("external_dirs_v1", "{}")
            .commit()
        assertTrue(NativeConfig.getGameDirs().isEmpty())
        assertTrue(NativeConfig.getExternalContentDirs().isEmpty())

        preferences.edit().putString("game_dirs_v1", """[
            {"uri":"content://games/one","deepScan":false,"type":"GAME"},
            null, 4, {"uri":12}, {"uri":""}, {},
            {"uri":"content://games/two","type":"OLD_TYPE"}
        ]""").putString("external_dirs_v1", """["content://external/one",42,null,""]""").commit()
        assertEquals(
            listOf(GameDir("content://games/one", false), GameDir("content://games/two", true)),
            NativeConfig.getGameDirs().toList()
        )
        assertEquals(listOf("content://external/one"), NativeConfig.getExternalContentDirs().toList())

        val dirs = arrayOf(GameDir("content://games/new", false, DirectoryType.EXTERNAL_CONTENT))
        NativeConfig.setGameDirs(dirs)
        assertEquals(dirs.toList(), NativeConfig.getGameDirs().toList())
        NativeConfig.setExternalContentDirs(arrayOf("content://external/new"))
        assertEquals(listOf("content://external/new"), NativeConfig.getExternalContentDirs().toList())
        preferences.edit().putInt("game_dirs_v1", 7).commit()
        assertTrue(NativeConfig.getGameDirs().isEmpty())
    }

    private fun launchSettings(app: YuzuApplication, game: Game?): org.robolectric.android.controller.ActivityController<SettingsActivity> {
        val controller = Robolectric.buildActivity(
            SettingsActivity::class.java,
            Intent(app, SettingsActivity::class.java).apply {
                putExtra("menuTag", Settings.MenuTag.SECTION_ROOT)
                putExtra("game", game)
            }
        ).setup()
        idleMainLooper()
        return controller
    }

    private fun currentSettingsFragment(controller: org.robolectric.android.controller.ActivityController<SettingsActivity>): SettingsFragment {
        idleMainLooper()
        val navHost = controller.get().supportFragmentManager
            .findFragmentById(R.id.fragment_container) as NavHostFragment
        assertEquals(R.id.settingsFragment, navHost.navController.currentDestination?.id)
        val fragment = navHost.childFragmentManager.primaryNavigationFragment
        assertNotNull(fragment)
        return fragment as SettingsFragment
    }

    private fun awaitRows(fragment: SettingsFragment): List<org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem> {
        val recycler = fragment.requireView().findViewById<RecyclerView>(R.id.list_settings)
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (System.nanoTime() < deadline) {
            idleMainLooper()
            val adapter = recycler.adapter as? SettingsAdapter
            if (adapter != null && adapter.currentList.isNotEmpty()) return adapter.currentList
            Thread.sleep(10)
        }
        throw AssertionError("Eden settings presenter did not populate rows")
    }

    private fun idleMainLooper() {
        shadowOf(Looper.getMainLooper()).idle()
    }

    private fun awaitVisible(fragment: androidx.fragment.app.Fragment, viewId: Int) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (System.nanoTime() < deadline) {
            idleMainLooper()
            val view = fragment.view?.findViewById<View>(viewId)
            if (view?.visibility == View.VISIBLE) return
            Thread.sleep(10)
        }
        throw AssertionError("Expected the empty library notice to become visible without keys")
    }
}
