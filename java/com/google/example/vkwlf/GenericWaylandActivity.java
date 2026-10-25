package com.google.example.vkwlf;

/**
 * Dynamic Android desktop window Activity slot A for arbitrary Wayland applications.
 */
public class GenericWaylandActivity extends MainActivity {
    @Override
    protected int getPreferredWindowSlot() {
        return 4;
    }
}
