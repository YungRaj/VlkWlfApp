#include "input_handler.h"

#include <chrono>

#include "log_macros.h"

namespace vkwlf {

namespace {

constexpr const char* kTag = "VkWlfCompositor";

// Linux input event codes (<linux/input-event-codes.h>)
constexpr uint32_t kBtnLeft = 0x110;
constexpr uint32_t kBtnRight = 0x111;
constexpr uint32_t kBtnMiddle = 0x112;

// Standard Android MotionEvent / KeyEvent constants for cross-platform compilation
#ifndef __ANDROID__
enum {
  AMOTION_EVENT_ACTION_MASK = 0xff,
  AMOTION_EVENT_ACTION_DOWN = 0,
  AMOTION_EVENT_ACTION_UP = 1,
  AMOTION_EVENT_ACTION_MOVE = 2,
  AMOTION_EVENT_ACTION_HOVER_MOVE = 7,
  AMOTION_EVENT_ACTION_SCROLL = 8,
  AMOTION_EVENT_ACTION_BUTTON_PRESS = 11,
  AMOTION_EVENT_ACTION_BUTTON_RELEASE = 12,
  AMOTION_EVENT_BUTTON_PRIMARY = 1 << 0,
  AMOTION_EVENT_BUTTON_SECONDARY = 1 << 1,
  AMOTION_EVENT_BUTTON_TERTIARY = 1 << 2,
  AKEY_EVENT_ACTION_DOWN = 0,
  AKEY_EVENT_ACTION_UP = 1,
  AMETA_SHIFT_ON = 0x01,
  AMETA_ALT_ON = 0x02,
  AMETA_CTRL_ON = 0x1000,
  AMETA_CAPS_LOCK_ON = 0x100000,
};
#endif

// XKB modifier bitmasks matching kMinimalXkbKeymap in wayland_server_engine.cpp
constexpr uint32_t kXkbModShift = 1 << 0;
constexpr uint32_t kXkbModLock = 1 << 1;
constexpr uint32_t kXkbModControl = 1 << 2;
constexpr uint32_t kXkbModMod1 = 1 << 3;  // Alt

}  // namespace

InputHandler::InputHandler(WaylandServerEngine* engine) : engine_(engine) {}

bool InputHandler::HandleAndroidInputEvent(const AInputEvent* event) {
#ifdef __ANDROID__
  if (!event || !engine_) return false;
  int32_t type = AInputEvent_getType(event);
  if (type == AINPUT_EVENT_TYPE_MOTION) {
    int32_t action = AMotionEvent_getAction(event);
    float x = AMotionEvent_getX(event, 0);
    float y = AMotionEvent_getY(event, 0);
    int32_t button_state = AMotionEvent_getButtonState(event);
    float vscroll = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0);
    float hscroll = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, 0);
    uint32_t time_ms = static_cast<uint32_t>(AMotionEvent_getEventTime(event) / 1000000);
    return OnAndroidMotionEvent(action, x, y, button_state, vscroll, hscroll, time_ms);
  } else if (type == AINPUT_EVENT_TYPE_KEY) {
    int32_t action = AKeyEvent_getAction(event);
    int32_t keycode = AKeyEvent_getKeyCode(event);
    int32_t meta = AKeyEvent_getMetaState(event);
    uint32_t time_ms = static_cast<uint32_t>(AKeyEvent_getEventTime(event) / 1000000);
    return OnAndroidKeyEvent(action, keycode, meta, time_ms);
  }
#else
  (void)event;
#endif
  return false;
}

bool InputHandler::OnAndroidMotionEvent(int32_t action,
                                        float x,
                                        float y,
                                        int32_t button_state,
                                        float vscroll,
                                        float hscroll,
                                        uint32_t time_ms) {
  if (!engine_) return false;
  int32_t masked_action = action & AMOTION_EVENT_ACTION_MASK;

  switch (masked_action) {
    case AMOTION_EVENT_ACTION_MOVE:
    case AMOTION_EVENT_ACTION_HOVER_MOVE:
      engine_->SendPointerMotion(time_ms, static_cast<double>(x), static_cast<double>(y));
      return true;

    case AMOTION_EVENT_ACTION_DOWN: {
      engine_->SendPointerMotion(time_ms, static_cast<double>(x), static_cast<double>(y));
      uint32_t btn = kBtnLeft;
      if (button_state & AMOTION_EVENT_BUTTON_SECONDARY) {
        btn = kBtnRight;
      } else if (button_state & AMOTION_EVENT_BUTTON_TERTIARY) {
        btn = kBtnMiddle;
      }
      engine_->SendPointerButton(time_ms, btn, WL_POINTER_BUTTON_STATE_PRESSED);
      last_button_state_ = button_state ? button_state : AMOTION_EVENT_BUTTON_PRIMARY;
      LOGD(kTag, "InputHandler: wl_pointer_send_button(btn=0x%x, state=PRESSED, x=%.1f, y=%.1f)",
           btn, x, y);
      return true;
    }

    case AMOTION_EVENT_ACTION_UP: {
      uint32_t btn = kBtnLeft;
      if (last_button_state_ & AMOTION_EVENT_BUTTON_SECONDARY) {
        btn = kBtnRight;
      } else if (last_button_state_ & AMOTION_EVENT_BUTTON_TERTIARY) {
        btn = kBtnMiddle;
      }
      engine_->SendPointerButton(time_ms, btn, WL_POINTER_BUTTON_STATE_RELEASED);
      last_button_state_ = 0;
      LOGD(kTag, "InputHandler: wl_pointer_send_button(btn=0x%x, state=RELEASED, x=%.1f, y=%.1f)",
           btn, x, y);
      return true;
    }

    case AMOTION_EVENT_ACTION_BUTTON_PRESS:
    case AMOTION_EVENT_ACTION_BUTTON_RELEASE: {
      uint32_t wl_state = (masked_action == AMOTION_EVENT_ACTION_BUTTON_PRESS)
                              ? WL_POINTER_BUTTON_STATE_PRESSED
                              : WL_POINTER_BUTTON_STATE_RELEASED;
      int32_t changed = button_state ^ last_button_state_;
      uint32_t btn = kBtnLeft;
      if (changed & AMOTION_EVENT_BUTTON_SECONDARY) {
        btn = kBtnRight;
      } else if (changed & AMOTION_EVENT_BUTTON_TERTIARY) {
        btn = kBtnMiddle;
      }
      last_button_state_ = button_state;
      engine_->SendPointerButton(time_ms, btn, wl_state);
      return true;
    }

    case AMOTION_EVENT_ACTION_SCROLL: {
      if (vscroll != 0.0f) {
        engine_->SendPointerAxis(time_ms, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 static_cast<double>(-vscroll * 15.0f));
      }
      if (hscroll != 0.0f) {
        engine_->SendPointerAxis(time_ms, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                 static_cast<double>(hscroll * 15.0f));
      }
      return true;
    }

    default:
      break;
  }
  return false;
}

bool InputHandler::OnAndroidKeyEvent(int32_t action,
                                     int32_t android_keycode,
                                     int32_t meta_state,
                                     uint32_t time_ms) {
  if (!engine_) return false;
  if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) {
    return false;
  }

  uint32_t evdev_key = AndroidKeycodeToLinuxEvdev(android_keycode);
  if (evdev_key == 0) {
    LOGW(kTag, "InputHandler: Unmapped Android keycode %d", android_keycode);
    return false;
  }

  uint32_t mods_depressed = 0;
  if (meta_state & AMETA_SHIFT_ON) mods_depressed |= kXkbModShift;
  if (meta_state & AMETA_CTRL_ON) mods_depressed |= kXkbModControl;
  if (meta_state & AMETA_ALT_ON) mods_depressed |= kXkbModMod1;

  uint32_t mods_locked = 0;
  if (meta_state & AMETA_CAPS_LOCK_ON) mods_locked |= kXkbModLock;

  if (mods_depressed != active_mods_depressed_ || mods_locked != active_mods_locked_) {
    active_mods_depressed_ = mods_depressed;
    active_mods_locked_ = mods_locked;
    engine_->SendKeyboardModifiers(active_mods_depressed_, 0, active_mods_locked_, 0);
  }

  uint32_t wl_state = (action == AKEY_EVENT_ACTION_DOWN) ? WL_KEYBOARD_KEY_STATE_PRESSED
                                                         : WL_KEYBOARD_KEY_STATE_RELEASED;
  engine_->SendKeyboardKey(time_ms, evdev_key, wl_state);
  LOGD(kTag,
       "InputHandler: wl_keyboard_send_key(android_key=%d -> evdev=%u, xkb=%u, state=%s, mods=0x%x)",
       android_keycode, evdev_key, evdev_key + 8,
       (wl_state == WL_KEYBOARD_KEY_STATE_PRESSED) ? "PRESSED" : "RELEASED",
       active_mods_depressed_);
  return true;
}

uint32_t InputHandler::AndroidKeycodeToLinuxEvdev(int32_t android_keycode) {
  // Android AKEYCODE_* values -> Linux evdev KEY_* codes (<linux/input-event-codes.h>)
  // Note: Wayland wl_keyboard.key takes the raw evdev keycode (XKB keycode = evdev + 8).
  switch (android_keycode) {
    case 7:  return 11;  // AKEYCODE_0 -> KEY_0
    case 8:  return 2;   // AKEYCODE_1 -> KEY_1
    case 9:  return 3;   // AKEYCODE_2 -> KEY_2
    case 10: return 4;   // AKEYCODE_3 -> KEY_3
    case 11: return 5;   // AKEYCODE_4 -> KEY_4
    case 12: return 6;   // AKEYCODE_5 -> KEY_5
    case 13: return 7;   // AKEYCODE_6 -> KEY_6
    case 14: return 8;   // AKEYCODE_7 -> KEY_7
    case 15: return 9;   // AKEYCODE_8 -> KEY_8
    case 16: return 10;  // AKEYCODE_9 -> KEY_9

    case 19: return 103; // AKEYCODE_DPAD_UP -> KEY_UP
    case 20: return 108; // AKEYCODE_DPAD_DOWN -> KEY_DOWN
    case 21: return 105; // AKEYCODE_DPAD_LEFT -> KEY_LEFT
    case 22: return 106; // AKEYCODE_DPAD_RIGHT -> KEY_RIGHT

    case 29: return 30;  // AKEYCODE_A -> KEY_A
    case 30: return 48;  // AKEYCODE_B -> KEY_B
    case 31: return 46;  // AKEYCODE_C -> KEY_C
    case 32: return 32;  // AKEYCODE_D -> KEY_D
    case 33: return 18;  // AKEYCODE_E -> KEY_E
    case 34: return 33;  // AKEYCODE_F -> KEY_F
    case 35: return 34;  // AKEYCODE_G -> KEY_G
    case 36: return 35;  // AKEYCODE_H -> KEY_H
    case 37: return 23;  // AKEYCODE_I -> KEY_I
    case 38: return 36;  // AKEYCODE_J -> KEY_J
    case 39: return 37;  // AKEYCODE_K -> KEY_K
    case 40: return 38;  // AKEYCODE_L -> KEY_L
    case 41: return 50;  // AKEYCODE_M -> KEY_M
    case 42: return 49;  // AKEYCODE_N -> KEY_N
    case 43: return 24;  // AKEYCODE_O -> KEY_O
    case 44: return 25;  // AKEYCODE_P -> KEY_P
    case 45: return 16;  // AKEYCODE_Q -> KEY_Q
    case 46: return 19;  // AKEYCODE_R -> KEY_R
    case 47: return 31;  // AKEYCODE_S -> KEY_S
    case 48: return 20;  // AKEYCODE_T -> KEY_T
    case 49: return 22;  // AKEYCODE_U -> KEY_U
    case 50: return 47;  // AKEYCODE_V -> KEY_V
    case 51: return 17;  // AKEYCODE_W -> KEY_W
    case 52: return 45;  // AKEYCODE_X -> KEY_X
    case 53: return 21;  // AKEYCODE_Y -> KEY_Y
    case 54: return 44;  // AKEYCODE_Z -> KEY_Z

    case 55: return 51;  // AKEYCODE_COMMA -> KEY_COMMA
    case 56: return 52;  // AKEYCODE_PERIOD -> KEY_DOT
    case 57: return 56;  // AKEYCODE_ALT_LEFT -> KEY_LEFTALT
    case 58: return 100; // AKEYCODE_ALT_RIGHT -> KEY_RIGHTALT
    case 59: return 42;  // AKEYCODE_SHIFT_LEFT -> KEY_LEFTSHIFT
    case 60: return 54;  // AKEYCODE_SHIFT_RIGHT -> KEY_RIGHTSHIFT
    case 61: return 15;  // AKEYCODE_TAB -> KEY_TAB
    case 62: return 57;  // AKEYCODE_SPACE -> KEY_SPACE
    case 66: return 28;  // AKEYCODE_ENTER -> KEY_ENTER
    case 67: return 14;  // AKEYCODE_DEL -> KEY_BACKSPACE
    case 68: return 41;  // AKEYCODE_GRAVE -> KEY_GRAVE
    case 69: return 12;  // AKEYCODE_MINUS -> KEY_MINUS
    case 70: return 13;  // AKEYCODE_EQUALS -> KEY_EQUAL
    case 71: return 26;  // AKEYCODE_LEFT_BRACKET -> KEY_LEFTBRACE
    case 72: return 27;  // AKEYCODE_RIGHT_BRACKET -> KEY_RIGHTBRACE
    case 73: return 43;  // AKEYCODE_BACKSLASH -> KEY_BACKSLASH
    case 74: return 39;  // AKEYCODE_SEMICOLON -> KEY_SEMICOLON
    case 75: return 40;  // AKEYCODE_APOSTROPHE -> KEY_APOSTROPHE
    case 76: return 53;  // AKEYCODE_SLASH -> KEY_SLASH
    case 92: return 104; // AKEYCODE_PAGE_UP -> KEY_PAGEUP
    case 93: return 109; // AKEYCODE_PAGE_DOWN -> KEY_PAGEDOWN
    case 111: return 1;  // AKEYCODE_ESCAPE -> KEY_ESC
    case 112: return 111;// AKEYCODE_FORWARD_DEL -> KEY_DELETE
    case 113: return 29; // AKEYCODE_CTRL_LEFT -> KEY_LEFTCTRL
    case 114: return 97; // AKEYCODE_CTRL_RIGHT -> KEY_RIGHTCTRL
    case 115: return 58; // AKEYCODE_CAPS_LOCK -> KEY_CAPSLOCK
    case 117: return 125;// AKEYCODE_META_LEFT -> KEY_LEFTMETA
    case 118: return 126;// AKEYCODE_META_RIGHT -> KEY_RIGHTMETA
    case 122: return 102;// AKEYCODE_MOVE_HOME -> KEY_HOME
    case 123: return 107;// AKEYCODE_MOVE_END -> KEY_END
    case 124: return 110;// AKEYCODE_INSERT -> KEY_INSERT
    case 131: return 59; // AKEYCODE_F1 -> KEY_F1
    case 132: return 60; // AKEYCODE_F2 -> KEY_F2
    case 133: return 61; // AKEYCODE_F3 -> KEY_F3
    case 134: return 62; // AKEYCODE_F4 -> KEY_F4
    case 135: return 63; // AKEYCODE_F5 -> KEY_F5
    case 136: return 64; // AKEYCODE_F6 -> KEY_F6
    case 137: return 65; // AKEYCODE_F7 -> KEY_F7
    case 138: return 66; // AKEYCODE_F8 -> KEY_F8
    case 139: return 67; // AKEYCODE_F9 -> KEY_F9
    case 140: return 68; // AKEYCODE_F10 -> KEY_F10
    case 141: return 87; // AKEYCODE_F11 -> KEY_F11
    case 142: return 88; // AKEYCODE_F12 -> KEY_F12
    default: return 0;
  }
}

}  // namespace vkwlf
