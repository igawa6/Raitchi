package com.recomp.letsgo.test;

import org.libsdl.app.SDLActivity;

/** Isolated engineering reference: SDL host only, no import/compiler or root helpers. */
public final class ReferenceActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
