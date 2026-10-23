package com.google.example.vkwlf;

/**
 * Dynamic native Android desktop window for multi-app Wayland rendering.
 * Uses documentLaunchMode="intoExisting" with per-surface document URIs (vkwlf://surface/<id>)
 * so any number (N) of Wayland applications can run side-by-side in separate native Android
 * desktop windows and switch via the OS Dock / Alt-Tab switcher.
 */
public class SecondaryWaylandActivity extends MainActivity {
    @Override
    protected int getPreferredWindowSlot() {
        return 1;
    }
}
