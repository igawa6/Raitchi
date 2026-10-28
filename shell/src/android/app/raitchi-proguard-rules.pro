# Stable engine JNI callbacks and SDL reflection contracts.
-keep class com.lasrecomp.android.** { *; }
-keep class org.libsdl.app.** { *; }
-keepnames class org.yuzu.yuzu_emu.model.Game
# Navigation inflates these serializable argument types by their XML class names.
-keep enum org.yuzu.yuzu_emu.features.settings.model.Settings$MenuTag { *; }
-keep enum org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen { *; }
-keepclassmembers class * implements android.os.Parcelable { public static ** CREATOR; }
