package com.google.example.vkwlf;

/**
 * Dedicated Android desktop window Activity for VS Code Wayland surfaces.
 */
public class VSCodeWaylandActivity extends MainActivity {
    @Override
    protected int getPreferredWindowSlot() {
        return 3;
    }
}
