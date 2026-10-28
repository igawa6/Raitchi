// SPDX-License-Identifier: GPL-3.0-or-later
plugins {
    id("com.android.application") version "8.9.1" apply false
    id("com.android.library") version "8.9.1" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
    id("kotlin-parcelize") version "1.9.24" apply false
    id("org.jetbrains.kotlin.plugin.serialization") version "1.9.24" apply false
    id("androidx.navigation.safeargs.kotlin") version "2.8.9" apply false
}
if (!hasProperty("raitchiWorkspace")) {
    throw GradleException("Build using switch-recomp-platform/tools/build_eden_shell.py")
}
allprojects {
    layout.buildDirectory.set(file("${rootProject.property("raitchiWorkspace")}/build/eden-shell/gradle/${project.name}"))
}
