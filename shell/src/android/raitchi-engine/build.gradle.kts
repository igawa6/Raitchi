// SPDX-License-Identifier: GPL-3.0-or-later
plugins { id("com.android.library") }
val workspace = file(rootProject.property("raitchiWorkspace")!!)
android {
    namespace = "com.lasrecomp.android"
    compileSdk = 36
    ndkVersion = "27.2.12479018"
    defaultConfig { minSdk = 30; ndk { abiFilters += "arm64-v8a" } }
    sourceSets.getByName("main") {
        java.srcDirs(workspace.resolve("work/shared-apk/app/src/main/java"),
            workspace.resolve("work/las-baseline/build/android/SDL3/android-project/app/src/main/java"))
        res.srcDir(workspace.resolve("work/shared-apk/app/src/main/res"))
        assets.srcDir(workspace.resolve("build/shared-android/apk-inputs/assets"))
        jniLibs.srcDir(workspace.resolve("build/shared-android/apk-inputs/jniLibs"))
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
