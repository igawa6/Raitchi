package com.lasrecomp.android;

import android.content.Context;

// The shared test app never probes root or changes device performance policy.
// Keep the pinned activity's call interface, with no script/process implementation.
final class RootBoost {
    static boolean enabled(Context context) { return false; }
    static synchronized void start(Context context) { }
    static synchronized void stop(Context context) { }
}
