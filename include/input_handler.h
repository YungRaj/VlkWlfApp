#pragma once

#include <cstdint>

#ifdef __ANDROID__
#include <android/input.h>
#include <android/keycodes.h>
#else
struct AInputEvent;
#endif

#include "wayland_server_engine.h"

namespace vkwlf {

class InputHandler {
 public:
  explicit InputHandler(WaylandServerEngine* engine);
  ~InputHandler() = default;

  // Entry point for Android NDK AInputEvent (from NativeActivity / AInputQueue).
  bool HandleAndroidInputEvent(const AInputEvent* event);

  // Direct translation methods used by both AInputEvent and JNI SurfaceView callbacks.
  bool OnAndroidMotionEvent(int32_t action,
                            float x,
                            float y,
                            int32_t button_state,
                            float vscroll,
                            float hscroll,
                            uint32_t time_ms);

  bool OnAndroidKeyEvent(int32_t action,
                         int32_t android_keycode,
                         int32_t meta_state,
                         uint32_t time_ms);

  // Translates an Android AKEYCODE_* value to a Linux evdev keycode (used by Wayland wl_keyboard).
  static uint32_t AndroidKeycodeToLinuxEvdev(int32_t android_keycode);

 private:
  WaylandServerEngine* engine_ = nullptr;
  int32_t last_button_state_ = 0;
  uint32_t active_mods_depressed_ = 0;
  uint32_t active_mods_locked_ = 0;
};

}  // namespace vkwlf
