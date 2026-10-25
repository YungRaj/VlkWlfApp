package com.google.example.vkwlf;

/**
 * Dynamic Android desktop window Activity slot B for arbitrary Wayland applications.
 */
public class GenericWaylandActivity2 extends MainActivity {
    @Override
    protected int getPreferredWindowSlot() {
        return 5;
    }
}
