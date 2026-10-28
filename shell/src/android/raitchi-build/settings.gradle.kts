// SPDX-License-Identifier: GPL-3.0-or-later
pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
    resolutionStrategy {
        eachPlugin {
            if (requested.id.id.startsWith("com.android.")) {
                useModule("com.android.tools.build:gradle:${requested.version}")
            } else if (requested.id.id == "org.jetbrains.kotlin.android" || requested.id.id == "kotlin-parcelize") {
                useModule("org.jetbrains.kotlin:kotlin-gradle-plugin:${requested.version}")
            } else if (requested.id.id == "org.jetbrains.kotlin.plugin.serialization") {
                useModule("org.jetbrains.kotlin:kotlin-serialization:${requested.version}")
            } else if (requested.id.id == "androidx.navigation.safeargs.kotlin") {
                useModule("androidx.navigation:navigation-safe-args-gradle-plugin:${requested.version}")
            }
        }
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories { google(); mavenCentral() }
}
rootProject.name = "Raitchi"
include(":app", ":raitchi-engine")
project(":app").projectDir = file("../app")
project(":raitchi-engine").projectDir = file("../raitchi-engine")
