// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright yuzu/Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Eden Android app module using the separately verified Raitchi engine.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("kotlin-parcelize")
    id("org.jetbrains.kotlin.plugin.serialization")
    id("androidx.navigation.safeargs.kotlin")
}
val retainedSources = file("raitchi-sources.txt").readLines().filter { it.isNotBlank() && !it.startsWith("#") }
android {
    namespace = "org.yuzu.yuzu_emu"
    compileSdk = 36
    ndkVersion = "27.2.12479018"
    defaultConfig {
        applicationId = "dev.igawa6.raitchi"
        minSdk = 30
        targetSdk = 36
        versionCode = 2
        versionName = "0.0.2"
        ndk { abiFilters += "arm64-v8a" }
        manifestPlaceholders += mapOf("appNameBase" to "Raitchi", "appNameSuffix" to "")
    }
    buildFeatures { viewBinding = true; buildConfig = true }
    testOptions { unitTests.isIncludeAndroidResources = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    signingConfigs {
        create("raitchiTest") {
            storeFile = file(providers.environmentVariable("RAITCHI_TEST_KEYSTORE").get())
            storePassword = providers.environmentVariable("RAITCHI_TEST_PASSWORD").get()
            keyAlias = "raitchi-test"
            keyPassword = storePassword
            enableV1Signing = false
            enableV2Signing = true
            enableV3Signing = true
        }
    }
    buildTypes {
        getByName("debug") { signingConfig = signingConfigs.getByName("raitchiTest") }
        getByName("release") {
            signingConfig = signingConfigs.getByName("raitchiTest")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "raitchi-proguard-rules.pro")
        }
    }
    packaging {
        jniLibs { useLegacyPackaging = true }
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
    sourceSets.getByName("main").assets.srcDir(rootProject.file("../../../LICENSES"))
    sourceSets.getByName("main").java.include(retainedSources)
}
kotlin { sourceSets.getByName("main").kotlin.include(retainedSources) }
dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.14.1")
    implementation(project(":raitchi-engine"))
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.recyclerview:recyclerview:1.4.0")
    implementation("androidx.constraintlayout:constraintlayout:2.2.1")
    implementation("androidx.fragment:fragment-ktx:1.8.6")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.8.7")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("io.coil-kt:coil:2.2.2")
    implementation("androidx.core:core-splashscreen:1.0.1")
    implementation("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
    implementation("androidx.swiperefreshlayout:swiperefreshlayout:1.1.0")
    implementation("org.commonmark:commonmark:0.22.0")
    implementation("androidx.navigation:navigation-fragment-ktx:2.8.9")
    implementation("androidx.navigation:navigation-ui-ktx:2.8.9")
    implementation("info.debatty:java-string-similarity:2.0.0")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.6.3")
}
