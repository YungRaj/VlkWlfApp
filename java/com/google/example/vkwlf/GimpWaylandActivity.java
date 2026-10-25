package com.google.example.vkwlf;

/**
 * Dedicated Android desktop window Activity for GIMP Wayland surfaces.
 */
public class GimpWaylandActivity extends MainActivity {
    @Override
    protected int getPreferredWindowSlot() {
        return 6;
    }
}
