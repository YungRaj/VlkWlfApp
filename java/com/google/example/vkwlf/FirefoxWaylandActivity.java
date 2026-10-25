package com.google.example.vkwlf;

/**
 * Dedicated Android desktop window Activity for Firefox Wayland surfaces.
 */
public class FirefoxWaylandActivity extends MainActivity {
    @Override
    protected int getPreferredWindowSlot() {
        return 2;
    }
}
