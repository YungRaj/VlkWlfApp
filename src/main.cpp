#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/input.h>
#include <android/native_activity.h>
#include <android/native_window_jni.h>
#include <jni.h>
#endif

#include "input_handler.h"
#include "log_macros.h"
#include "vk_renderer.h"
#include "wayland_server_engine.h"

namespace {

constexpr const char* kTag = "VkWlfCompositor";
std::atomic<bool> g_keep_running{true};

void SignalHandler(int /*sig*/) { g_keep_running.store(false); }

struct WindowSlot {
  ANativeWindow* window = nullptr;
  std::shared_ptr<vkwlf::VkRenderer> renderer;
  uint64_t window_seq = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t target_surface_id = 0;
  bool user_pinned_surface = false;
#ifdef __ANDROID__
  jobject activity_global = nullptr;
  std::string last_notified_app_key;
  bool last_notified_has_icon = false;
#endif
};

struct AppBridgeState {
  std::unique_ptr<vkwlf::WaylandServerEngine> server;
  std::unique_ptr<vkwlf::InputHandler> input;
  std::mutex slots_mutex;
  std::map<int, WindowSlot> slots;
  std::atomic<int> next_dynamic_slot_id{8};
  std::thread pump_thread;
  std::atomic<bool> pump_running{false};
  std::mutex cache_mutex;
  std::map<uint32_t, vkwlf::CommittedFrame> cached_frames;
#ifdef __ANDROID__
  JavaVM* jvm = nullptr;
  jobject app_launch_listener_global = nullptr;
#endif
};

AppBridgeState g_app;

void CloseFrameFds(vkwlf::CommittedFrame& frame) {
  if (frame.dmabuf_fd >= 0) {
    close(frame.dmabuf_fd);
    frame.dmabuf_fd = -1;
  }
  for (auto& p : frame.planes) {
    if (p.fd >= 0) {
      close(p.fd);
      p.fd = -1;
    }
  }
}

vkwlf::CommittedFrame CloneFrameForRenderer(const vkwlf::CommittedFrame& src,
                                            bool include_icon = false) {
  vkwlf::CommittedFrame copy;
  copy.surface_id = src.surface_id;
  copy.parent_surface_id = src.parent_surface_id;
  copy.frame_number = src.frame_number;
  copy.width = src.width;
  copy.height = src.height;
  copy.stride = src.stride;
  copy.format = src.format;
  copy.buffer_type = src.buffer_type;
  if (src.dmabuf_fd < 0) {
    copy.shm_pixels = src.shm_pixels;
  }
  copy.dmabuf_fd = (src.dmabuf_fd >= 0) ? dup(src.dmabuf_fd) : -1;
  copy.drm_modifier = src.drm_modifier;
  copy.x = src.x;
  copy.y = src.y;
  copy.is_popup = src.is_popup;
  copy.is_subsurface = src.is_subsurface;
  copy.is_unmapped = src.is_unmapped;
  copy.title = src.title;
  copy.app_id = src.app_id;
  if (include_icon) {
    copy.icon_png = src.icon_png;
  }
  copy.planes.reserve(src.planes.size());
  for (const auto& p : src.planes) {
    copy.planes.push_back(vkwlf::DmaBufPlane{
        .fd = (p.fd >= 0) ? dup(p.fd) : -1,
        .offset = p.offset,
        .stride = p.stride,
        .modifier = p.modifier,
    });
  }
  return copy;
}

std::vector<vkwlf::CommittedFrame> CollectReplayFramesForTargetLocked(uint32_t target_sid) {
  std::vector<vkwlf::CommittedFrame> frames;
  // Pass 1: Parent/base toplevel frames first so child subsurfaces (even with lower surface IDs,
  // e.g. Firefox's WebRender subsurface sid=10 under xdg_toplevel sid=13) render on top.
  for (const auto& [sid, cached] : g_app.cached_frames) {
    if (target_sid == 0) {
      if (!cached.is_subsurface && cached.parent_surface_id == 0) {
        frames.push_back(CloneFrameForRenderer(cached));
      }
    } else if (sid == target_sid) {
      frames.push_back(CloneFrameForRenderer(cached));
    }
  }
  // Pass 2: Child subsurfaces/popups second.
  for (const auto& [sid, cached] : g_app.cached_frames) {
    if (target_sid == 0) {
      if (cached.is_subsurface || cached.parent_surface_id != 0) {
        frames.push_back(CloneFrameForRenderer(cached));
      }
    } else if (sid != target_sid && cached.parent_surface_id == target_sid) {
      frames.push_back(CloneFrameForRenderer(cached));
    }
  }
  return frames;
}

std::string CanonicalAppKey(const vkwlf::CommittedFrame& f) {
  std::string app_low = f.app_id;
  for (char& c : app_low) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  std::string title_low = f.title;
  for (char& c : title_low) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  std::string combined = app_low + " " + title_low;
  if (combined.find("teamviewer") != std::string::npos) return "teamviewer";
  if (combined.find("antigravity") != std::string::npos) return "antigravity";
  if (combined.find("firefox") != std::string::npos) return "firefox";
  if (combined.find("gimp") != std::string::npos ||
      combined.find("image manipulation") != std::string::npos) {
    return "gimp";
  }
  if (combined.find("vscode") != std::string::npos ||
      combined.find("visual studio code") != std::string::npos || app_low == "code") {
    return "vscode";
  }
  if (!app_low.empty() && app_low != "xwayland" && app_low != "wlroots") {
    return app_low;
  }
  if (!title_low.empty()) {
    size_t dash = title_low.find(" - ");
    if (dash != std::string::npos && dash > 0) {
      return title_low.substr(0, dash);
    }
    return title_low;
  }
  return "";
}

bool IsBaseToplevelFrame(const vkwlf::CommittedFrame& f) {
  return !f.is_popup && !f.is_subsurface && !f.is_unmapped && f.parent_surface_id == 0 &&
         f.width >= 320 && f.height >= 240;
}

void NotifySlotAppInfo(int slot_idx,
                       uint32_t surface_id,
                       const std::string& title,
                       const std::string& app_id,
                       const std::vector<uint8_t>& icon_png,
                       bool force_notify = false) {
#ifdef __ANDROID__
  if (!g_app.jvm || slot_idx < 0) return;
  JNIEnv* env = nullptr;
  bool attached = false;
  if (g_app.jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (g_app.jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    }
  }
  if (!env) return;

  jobject activity_ref = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_app.slots_mutex);
    auto it = g_app.slots.find(slot_idx);
    if (it == g_app.slots.end() || !it->second.activity_global) {
      if (attached) g_app.jvm->DetachCurrentThread();
      return;
    }
    WindowSlot& slot = it->second;
    const std::string app_key =
        std::to_string(surface_id) + "|" + (!app_id.empty() ? app_id : title) + "|" + title;
    const bool has_icon = !icon_png.empty();
    const bool need_update =
        force_notify || (app_key != slot.last_notified_app_key) ||
        (!slot.last_notified_has_icon && has_icon);
    if (!need_update) {
      if (attached) g_app.jvm->DetachCurrentThread();
      return;
    }
    slot.last_notified_app_key = app_key;
    if (has_icon) {
      slot.last_notified_has_icon = true;
    }
    activity_ref = env->NewLocalRef(slot.activity_global);
  }

  if (activity_ref) {
    jstring j_title = env->NewStringUTF(title.c_str());
    jstring j_app_id = env->NewStringUTF(app_id.c_str());
    jbyteArray j_icon = nullptr;
    if (!icon_png.empty()) {
      j_icon = env->NewByteArray(static_cast<jsize>(icon_png.size()));
      if (j_icon) {
        env->SetByteArrayRegion(j_icon, 0, static_cast<jsize>(icon_png.size()),
                                reinterpret_cast<const jbyte*>(icon_png.data()));
      }
    }

    jclass act_cls = env->GetObjectClass(activity_ref);
    if (act_cls) {
      jmethodID mid = env->GetMethodID(
          act_cls, "onWaylandAppInfo", "(ILjava/lang/String;Ljava/lang/String;[B)V");
      if (mid) {
        env->CallVoidMethod(activity_ref, mid, static_cast<jint>(surface_id), j_title, j_app_id,
                            j_icon);
      } else {
        env->ExceptionClear();
      }
      env->DeleteLocalRef(act_cls);
    }

    if (j_icon) env->DeleteLocalRef(j_icon);
    env->DeleteLocalRef(j_title);
    env->DeleteLocalRef(j_app_id);
    env->DeleteLocalRef(activity_ref);
  }

  if (attached) {
    g_app.jvm->DetachCurrentThread();
  }
#else
  (void)slot_idx;
  (void)surface_id;
  (void)title;
  (void)app_id;
  (void)icon_png;
  (void)force_notify;
#endif
}

void NotifyAllActivitiesAppsList() {
#ifdef __ANDROID__
  if (!g_app.jvm) return;
  JNIEnv* env = nullptr;
  bool attached = false;
  if (g_app.jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (g_app.jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    }
  }
  if (!env) return;

  struct AppEntry {
    uint32_t surface_id;
    std::string title;
    std::string app_id;
  };
  std::vector<AppEntry> apps;
  {
    std::lock_guard<std::mutex> lock(g_app.cache_mutex);
    for (const auto& [sid, frame] : g_app.cached_frames) {
      if (IsBaseToplevelFrame(frame)) {
        apps.push_back(AppEntry{sid, frame.title, frame.app_id});
      }
    }
  }

  jsize count = static_cast<jsize>(apps.size());
  jintArray j_sids = env->NewIntArray(count);
  jclass str_cls = env->FindClass("java/lang/String");
  jobjectArray j_titles = env->NewObjectArray(count, str_cls, env->NewStringUTF(""));
  jobjectArray j_app_ids = env->NewObjectArray(count, str_cls, env->NewStringUTF(""));

  std::vector<jint> sid_vals(apps.size());
  for (size_t i = 0; i < apps.size(); ++i) {
    sid_vals[i] = static_cast<jint>(apps[i].surface_id);
    jstring jt = env->NewStringUTF(apps[i].title.c_str());
    jstring ja = env->NewStringUTF(apps[i].app_id.c_str());
    env->SetObjectArrayElement(j_titles, static_cast<jsize>(i), jt);
    env->SetObjectArrayElement(j_app_ids, static_cast<jsize>(i), ja);
    env->DeleteLocalRef(jt);
    env->DeleteLocalRef(ja);
  }
  if (count > 0) {
    env->SetIntArrayRegion(j_sids, 0, count, sid_vals.data());
  }

  std::vector<jobject> activity_refs;
  std::vector<jint> bound_sids;
  {
    std::lock_guard<std::mutex> lock(g_app.slots_mutex);
    for (const auto& [slot_id, slot] : g_app.slots) {
      (void)slot_id;
      if (slot.activity_global) {
        activity_refs.push_back(env->NewLocalRef(slot.activity_global));
        bound_sids.push_back(static_cast<jint>(slot.target_surface_id));
      }
    }
  }

  for (size_t i = 0; i < activity_refs.size(); ++i) {
    jobject act = activity_refs[i];
    if (!act) continue;
    jclass act_cls = env->GetObjectClass(act);
    if (act_cls) {
      jmethodID mid = env->GetMethodID(
          act_cls, "onWaylandAppsChanged", "([I[Ljava/lang/String;[Ljava/lang/String;I)V");
      if (mid) {
        env->CallVoidMethod(act, mid, j_sids, j_titles, j_app_ids, bound_sids[i]);
      } else {
        env->ExceptionClear();
      }
      env->DeleteLocalRef(act_cls);
    }
    env->DeleteLocalRef(act);
  }

  env->DeleteLocalRef(j_sids);
  env->DeleteLocalRef(j_titles);
  env->DeleteLocalRef(j_app_ids);
  env->DeleteLocalRef(str_cls);

  if (attached) {
    g_app.jvm->DetachCurrentThread();
  }
#endif
}

void UpdateSlotAssignmentsAndNotify() {
  std::vector<vkwlf::CommittedFrame> base_apps;
  {
    std::lock_guard<std::mutex> lock(g_app.cache_mutex);
    for (const auto& [sid, frame] : g_app.cached_frames) {
      if (IsBaseToplevelFrame(frame)) {
        base_apps.push_back(frame);
      }
    }
  }
  if (base_apps.empty()) return;

  auto is_primary_ide = [](const vkwlf::CommittedFrame& f) {
    return f.app_id.find("antigravity") != std::string::npos ||
           f.title.find("Antigravity") != std::string::npos;
  };

  // Sort base apps so primary IDE (Antigravity) naturally orders first for Slot 0
  // and secondary apps (TeamViewer, etc.) order in deterministic surface_id order.
  std::sort(base_apps.begin(), base_apps.end(),
            [&is_primary_ide](const vkwlf::CommittedFrame& a, const vkwlf::CommittedFrame& b) {
              bool ide_a = is_primary_ide(a);
              bool ide_b = is_primary_ide(b);
              if (ide_a != ide_b) return ide_a;
              return a.surface_id < b.surface_id;
            });

  struct PendingNotification {
    int slot_idx;
    uint32_t surface_id;
    std::string title;
    std::string app_id;
    std::vector<uint8_t> icon_png;
    bool changed;
  };
  std::vector<PendingNotification> notifications;

  struct PendingReplay {
    std::shared_ptr<vkwlf::VkRenderer> renderer;
    uint32_t surface_id;
  };
  std::vector<PendingReplay> pending_replays;

  struct PendingResize {
    uint32_t surface_id;
    uint32_t width;
    uint32_t height;
  };
  std::vector<PendingResize> pending_resizes;

  {
    std::lock_guard<std::mutex> lock(g_app.slots_mutex);
    std::set<uint32_t> claimed_sids;
    // Pass 1: Honor any slot whose currently bound surface is still active so multi-window apps
    // never involuntarily jump between surfaces.
    for (auto& [slot_id, slot] : g_app.slots) {
      (void)slot_id;
      if (slot.target_surface_id != 0) {
        for (const auto& app : base_apps) {
          if (app.surface_id == slot.target_surface_id) {
            claimed_sids.insert(slot.target_surface_id);
            slot.user_pinned_surface = true;
            break;
          }
        }
      }
    }

    // Pass 2: Assign each unpinned or invalidated slot to its matching or unclaimed Wayland app.
    size_t ordinal = 0;
    for (auto& [slot_id, slot] : g_app.slots) {
      bool current_valid = false;
      for (const auto& app : base_apps) {
        if (app.surface_id == slot.target_surface_id) {
          current_valid = true;
          break;
        }
      }
      uint32_t desired_sid = slot.target_surface_id;
      if (!current_valid || !slot.user_pinned_surface) {
        uint32_t picked = 0;
        const bool is_dedicated_secondary_slot =
            (slot_id == 1 || slot_id == 2 || slot_id == 3 || slot_id == 6);
        for (const auto& app : base_apps) {
          std::string key = CanonicalAppKey(app);
          bool matches_slot = false;
          if (slot_id == 0) {
            matches_slot = (key == "antigravity");
          } else if (slot_id == 1) {
            matches_slot = (key == "teamviewer");
          } else if (slot_id == 2) {
            matches_slot = (key == "firefox");
          } else if (slot_id == 3) {
            matches_slot = (key == "vscode");
          } else if (slot_id == 6) {
            matches_slot = (key == "gimp");
          } else {
            matches_slot = (key != "antigravity" && key != "teamviewer" &&
                            key != "firefox" && key != "vscode" && key != "gimp");
          }
          if (matches_slot && claimed_sids.find(app.surface_id) == claimed_sids.end()) {
            picked = app.surface_id;
            claimed_sids.insert(picked);
            if (is_dedicated_secondary_slot || slot_id == 0) {
              slot.user_pinned_surface = true;
            }
            break;
          }
        }
        if (picked == 0 && !is_dedicated_secondary_slot) {
          for (const auto& app : base_apps) {
            if (claimed_sids.find(app.surface_id) == claimed_sids.end()) {
              picked = app.surface_id;
              claimed_sids.insert(picked);
              break;
            }
          }
        }
        if (picked == 0 && !is_dedicated_secondary_slot) {
          picked = base_apps[ordinal % base_apps.size()].surface_id;
        }
        desired_sid = picked;
      }
      ordinal++;
      bool sid_changed = (desired_sid != slot.target_surface_id);
      slot.target_surface_id = desired_sid;
      if (slot.renderer) {
        slot.renderer->SetTargetSurfaceId(desired_sid);
        if (sid_changed && desired_sid != 0) {
          pending_replays.push_back(PendingReplay{slot.renderer, desired_sid});
        }
      }
      if (sid_changed && desired_sid != 0 && slot.window != nullptr && slot.width > 0 &&
          slot.height > 0) {
        pending_resizes.push_back(PendingResize{desired_sid, slot.width, slot.height});
      }
      for (const auto& app : base_apps) {
        if (app.surface_id == desired_sid) {
          notifications.push_back(PendingNotification{
              slot_id, app.surface_id, app.title, app.app_id, app.icon_png, sid_changed});
          break;
        }
      }
    }
  }

  if (g_app.server) {
    for (const auto& pr : pending_resizes) {
      g_app.server->SetActiveInputSurface(pr.surface_id, pr.width, pr.height);
      g_app.server->SetDisplaySizeForSurface(pr.surface_id, pr.width, pr.height);
    }
  }

  for (auto& pr : pending_replays) {
    std::vector<vkwlf::CommittedFrame> replay_frames;
    {
      std::lock_guard<std::mutex> lock(g_app.cache_mutex);
      replay_frames = CollectReplayFramesForTargetLocked(pr.surface_id);
    }
    if (!replay_frames.empty()) {
      std::thread([renderer = std::move(pr.renderer),
                   frames = std::move(replay_frames)]() mutable {
        for (const auto& f : frames) {
          renderer->ProcessAndPresentFrame(f);
        }
      }).detach();
    }
  }

  for (const auto& n : notifications) {
    NotifySlotAppInfo(n.slot_idx, n.surface_id, n.title, n.app_id, n.icon_png, n.changed);
  }
  NotifyAllActivitiesAppsList();
}

void NotifyExternalAppLaunchListener(uint32_t surface_id,
                                     const std::string& title,
                                     const std::string& app_id,
                                     const std::vector<uint8_t>& icon_png) {
#ifdef __ANDROID__
  if (!g_app.jvm || !g_app.app_launch_listener_global) return;
  JNIEnv* env = nullptr;
  bool attached = false;
  if (g_app.jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (g_app.jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    }
  }
  if (!env) return;

  jstring j_title = env->NewStringUTF(title.c_str());
  jstring j_app_id = env->NewStringUTF(app_id.c_str());
  jbyteArray j_icon = nullptr;
  if (!icon_png.empty()) {
    j_icon = env->NewByteArray(static_cast<jsize>(icon_png.size()));
    if (j_icon) {
      env->SetByteArrayRegion(j_icon, 0, static_cast<jsize>(icon_png.size()),
                              reinterpret_cast<const jbyte*>(icon_png.data()));
    }
  }

  jclass cls = env->GetObjectClass(g_app.app_launch_listener_global);
  if (cls) {
    jmethodID mid_with_icon = env->GetMethodID(
        cls, "onWaylandAppLaunched", "(ILjava/lang/String;Ljava/lang/String;[B)V");
    if (mid_with_icon) {
      env->CallVoidMethod(g_app.app_launch_listener_global, mid_with_icon,
                          static_cast<jint>(surface_id), j_title, j_app_id, j_icon);
    } else {
      env->ExceptionClear();
      jmethodID mid = env->GetMethodID(
          cls, "onWaylandAppLaunched", "(ILjava/lang/String;Ljava/lang/String;)V");
      if (mid) {
        env->CallVoidMethod(g_app.app_launch_listener_global, mid,
                            static_cast<jint>(surface_id), j_title, j_app_id);
      } else {
        env->ExceptionClear();
      }
    }
    env->DeleteLocalRef(cls);
  }

  if (j_icon) env->DeleteLocalRef(j_icon);
  env->DeleteLocalRef(j_title);
  env->DeleteLocalRef(j_app_id);

  if (attached) {
    g_app.jvm->DetachCurrentThread();
  }
#else
  (void)surface_id;
  (void)title;
  (void)app_id;
  (void)icon_png;
#endif
}

void NotifyAllActivitiesGuestCatalog(const std::vector<vkwlf::GuestAppCatalogEntry>& entries) {
#ifdef __ANDROID__
  if (!g_app.jvm) return;
  JNIEnv* env = nullptr;
  bool attached = false;
  if (g_app.jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (g_app.jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    }
  }
  if (!env) return;

  jsize count = static_cast<jsize>(entries.size());
  jclass str_cls = env->FindClass("java/lang/String");
  jobjectArray j_ids = env->NewObjectArray(count, str_cls, env->NewStringUTF(""));
  jobjectArray j_names = env->NewObjectArray(count, str_cls, env->NewStringUTF(""));
  jobjectArray j_comments = env->NewObjectArray(count, str_cls, env->NewStringUTF(""));
  jobjectArray j_execs = env->NewObjectArray(count, str_cls, env->NewStringUTF(""));
  jobjectArray j_apts = env->NewObjectArray(count, str_cls, env->NewStringUTF(""));
  jbooleanArray j_installed = env->NewBooleanArray(count);

  std::vector<jboolean> inst_vals(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    inst_vals[i] = entries[i].installed ? JNI_TRUE : JNI_FALSE;
    jstring jid = env->NewStringUTF(entries[i].desktop_id.c_str());
    jstring jn = env->NewStringUTF(entries[i].name.c_str());
    jstring jc = env->NewStringUTF(entries[i].comment.c_str());
    jstring je = env->NewStringUTF(entries[i].exec.c_str());
    jstring ja = env->NewStringUTF(entries[i].apt_package.c_str());
    env->SetObjectArrayElement(j_ids, static_cast<jsize>(i), jid);
    env->SetObjectArrayElement(j_names, static_cast<jsize>(i), jn);
    env->SetObjectArrayElement(j_comments, static_cast<jsize>(i), jc);
    env->SetObjectArrayElement(j_execs, static_cast<jsize>(i), je);
    env->SetObjectArrayElement(j_apts, static_cast<jsize>(i), ja);
    env->DeleteLocalRef(jid);
    env->DeleteLocalRef(jn);
    env->DeleteLocalRef(jc);
    env->DeleteLocalRef(je);
    env->DeleteLocalRef(ja);
  }
  if (count > 0) {
    env->SetBooleanArrayRegion(j_installed, 0, count, inst_vals.data());
  }

  std::vector<jobject> activity_refs;
  {
    std::lock_guard<std::mutex> lock(g_app.slots_mutex);
    for (const auto& [slot_id, slot] : g_app.slots) {
      (void)slot_id;
      if (slot.activity_global) {
        activity_refs.push_back(env->NewLocalRef(slot.activity_global));
      }
    }
  }

  for (jobject act : activity_refs) {
    if (!act) continue;
    jclass act_cls = env->GetObjectClass(act);
    if (act_cls) {
      jmethodID mid = env->GetMethodID(
          act_cls, "onGuestAppCatalogUpdated",
          "([Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[Z)V");
      if (mid) {
        env->CallVoidMethod(act, mid, j_ids, j_names, j_comments, j_execs, j_apts, j_installed);
      } else {
        env->ExceptionClear();
      }
      env->DeleteLocalRef(act_cls);
    }
    env->DeleteLocalRef(act);
  }

  env->DeleteLocalRef(j_ids);
  env->DeleteLocalRef(j_names);
  env->DeleteLocalRef(j_comments);
  env->DeleteLocalRef(j_execs);
  env->DeleteLocalRef(j_apts);
  env->DeleteLocalRef(j_installed);
  env->DeleteLocalRef(str_cls);

  if (attached) {
    g_app.jvm->DetachCurrentThread();
  }
#else
  (void)entries;
#endif
}

void EnsureServerAndPumpStarted(const std::string& socket_path,
                                int vsock_port,
                                int tcp_port,
                                uint32_t width,
                                uint32_t height) {
  if (!g_app.server) {
    vkwlf::WaylandServerEngine::Config srv_cfg;
    srv_cfg.socket_path = socket_path;
    srv_cfg.vsock_port = vsock_port;
    srv_cfg.tcp_port = tcp_port;
    srv_cfg.display_width = width;
    srv_cfg.display_height = height;

    g_app.server = std::make_unique<vkwlf::WaylandServerEngine>(srv_cfg);
    g_app.server->SetAppLaunchedCallback(
        [](uint32_t surface_id, const std::string& title, const std::string& app_id,
           const std::vector<uint8_t>& icon_png) {
          LOGI(kTag,
               "[AutoForeground] Guest Wayland application launched/updated: surface=%u title='%s' "
               "app_id='%s' icon_bytes=%zu",
               surface_id, title.c_str(), app_id.c_str(), icon_png.size());
          NotifyExternalAppLaunchListener(surface_id, title, app_id, icon_png);
        });
    g_app.server->SetAppCatalogCallback(
        [](const std::vector<vkwlf::GuestAppCatalogEntry>& entries) {
          NotifyAllActivitiesGuestCatalog(entries);
        });

    if (g_app.server->Initialize()) {
      g_app.server->Start();
    }
    g_app.input = std::make_unique<vkwlf::InputHandler>(g_app.server.get());
  }

  if (!g_app.pump_running.exchange(true)) {
    g_app.pump_thread = std::thread([]() {
      std::set<uint32_t> seen_base_toplevel_sids;
      while (g_app.pump_running.load()) {
        vkwlf::CommittedFrame frame;
        if (!g_app.server || !g_app.server->PopCommittedFrame(&frame, 16)) {
          continue;
        }
        bool topology_or_meta_changed = false;
        uint32_t newly_opened_same_app_sid = 0;
        uint32_t newly_opened_resize_w = 0;
        uint32_t newly_opened_resize_h = 0;
        {
          std::lock_guard<std::mutex> lock(g_app.cache_mutex);
          if (frame.is_unmapped) {
            seen_base_toplevel_sids.erase(frame.surface_id);
            auto it = g_app.cached_frames.find(frame.surface_id);
            if (it != g_app.cached_frames.end()) {
              if (it->second.dmabuf_fd >= 0) close(it->second.dmabuf_fd);
              for (auto& p : it->second.planes) {
                if (p.fd >= 0) close(p.fd);
              }
              g_app.cached_frames.erase(it);
              topology_or_meta_changed = true;
            }
          } else if (!frame.shm_pixels.empty() || frame.dmabuf_fd >= 0) {
            auto it = g_app.cached_frames.find(frame.surface_id);
            const bool is_first_base_toplevel_commit =
                IsBaseToplevelFrame(frame) &&
                seen_base_toplevel_sids.insert(frame.surface_id).second;
            if (IsBaseToplevelFrame(frame)) {
              std::string new_key = CanonicalAppKey(frame);
              if (!new_key.empty()) {
                // Inherit app icon from any existing window of the same app if missing,
                // while keeping all live windows of the same app in cached_frames so
                // multiple windows per app coexist without ping-ponging.
                for (const auto& [other_sid, other_frame] : g_app.cached_frames) {
                  if (other_sid != frame.surface_id && IsBaseToplevelFrame(other_frame) &&
                      CanonicalAppKey(other_frame) == new_key) {
                    if (frame.icon_png.empty() && !other_frame.icon_png.empty()) {
                      frame.icon_png = other_frame.icon_png;
                      break;
                    }
                  }
                }
                if (is_first_base_toplevel_commit) {
                  std::lock_guard<std::mutex> slock(g_app.slots_mutex);
                  for (auto& [sid_slot, s] : g_app.slots) {
                    (void)sid_slot;
                    if (s.target_surface_id != 0 && s.target_surface_id != frame.surface_id) {
                      auto old_it = g_app.cached_frames.find(s.target_surface_id);
                      if (old_it != g_app.cached_frames.end() &&
                          CanonicalAppKey(old_it->second) == new_key) {
                        s.target_surface_id = frame.surface_id;
                        s.user_pinned_surface = true;
                        if (s.renderer) {
                          s.renderer->SetTargetSurfaceId(frame.surface_id);
                        }
                        newly_opened_same_app_sid = frame.surface_id;
                        if (s.window != nullptr && s.width > 0 && s.height > 0) {
                          newly_opened_resize_w = s.width;
                          newly_opened_resize_h = s.height;
                        }
                        topology_or_meta_changed = true;
                        break;
                      }
                    }
                  }
                }
              }
            }
            if (it == g_app.cached_frames.end()) {
              topology_or_meta_changed = true;
            } else {
              if (frame.icon_png.empty() && !it->second.icon_png.empty()) {
                frame.icon_png = it->second.icon_png;
              } else if (!frame.icon_png.empty() && it->second.icon_png.empty()) {
                topology_or_meta_changed = true;
              }
              if (frame.title != it->second.title || frame.app_id != it->second.app_id) {
                topology_or_meta_changed = true;
              }
              if (it->second.dmabuf_fd >= 0) close(it->second.dmabuf_fd);
              for (auto& p : it->second.planes) {
                if (p.fd >= 0) close(p.fd);
              }
            }
            g_app.cached_frames[frame.surface_id] = CloneFrameForRenderer(frame, true);
          }
        }

        if (newly_opened_same_app_sid != 0 && g_app.server) {
          if (newly_opened_resize_w > 0 && newly_opened_resize_h > 0) {
            g_app.server->SetActiveInputSurface(newly_opened_same_app_sid, newly_opened_resize_w,
                                                newly_opened_resize_h);
            g_app.server->SetDisplaySizeForSurface(newly_opened_same_app_sid, newly_opened_resize_w,
                                                   newly_opened_resize_h);
          } else {
            g_app.server->SetActiveInputSurface(newly_opened_same_app_sid);
          }
        }

        bool any_slot_unassigned = false;
        {
          std::lock_guard<std::mutex> lock(g_app.slots_mutex);
          for (const auto& [slot_id, slot] : g_app.slots) {
            (void)slot_id;
            if (slot.target_surface_id == 0) {
              any_slot_unassigned = true;
              break;
            }
          }
        }
        if (topology_or_meta_changed || (any_slot_unassigned && IsBaseToplevelFrame(frame))) {
          UpdateSlotAssignmentsAndNotify();
        }

        struct SlotRenderTarget {
          int slot_id;
          std::shared_ptr<vkwlf::VkRenderer> renderer;
          uint32_t target_surface_id;
        };
        std::vector<SlotRenderTarget> targets;
        {
          std::lock_guard<std::mutex> lock(g_app.slots_mutex);
          for (auto& [slot_id, slot] : g_app.slots) {
            if (!slot.renderer) continue;
            if (slot.target_surface_id != 0 && frame.surface_id != slot.target_surface_id &&
                frame.parent_surface_id != slot.target_surface_id) {
              continue;
            }
            targets.push_back(SlotRenderTarget{slot_id, slot.renderer, slot.target_surface_id});
          }
        }

        for (auto& t : targets) {
          t.renderer->SetTargetSurfaceId(t.target_surface_id);
          vkwlf::CommittedFrame slot_frame = CloneFrameForRenderer(frame);
          t.renderer->ProcessAndPresentFrame(slot_frame);
          auto stats = t.renderer->GetStats();
          if (stats.presents_succeeded <= 5 || (stats.presents_succeeded % 60 == 0)) {
            LOGI(kTag,
                 "[HostGPU][Slot %d] Presented Wayland frame #%llu (surface=%u, target=%u, "
                 "buffer_type=%s, shm_uploads=%llu, dmabuf_imports=%llu, presents=%llu)",
                 t.slot_id, static_cast<unsigned long long>(frame.frame_number), frame.surface_id,
                 t.target_surface_id,
                 frame.buffer_type == vkwlf::BufferType::kDmaBuf ? "DMA-BUF/AHB" : "SHM",
                 static_cast<unsigned long long>(stats.shm_uploads),
                 static_cast<unsigned long long>(stats.dmabuf_imports),
                 static_cast<unsigned long long>(stats.presents_succeeded));
          }
        }

        if (frame.dmabuf_fd >= 0) close(frame.dmabuf_fd);
        for (auto& p : frame.planes) {
          if (p.fd >= 0) close(p.fd);
        }
      }
    });
  }
}

int AllocateWindowSlot(int preferred_slot) {
  std::lock_guard<std::mutex> lock(g_app.slots_mutex);
  if (preferred_slot >= 0) {
    auto it = g_app.slots.find(preferred_slot);
    if (it == g_app.slots.end() || it->second.window == nullptr) {
      g_app.slots[preferred_slot];
      return preferred_slot;
    }
  }
  int slot_id = g_app.next_dynamic_slot_id.fetch_add(1);
  while (g_app.slots.find(slot_id) != g_app.slots.end()) {
    slot_id = g_app.next_dynamic_slot_id.fetch_add(1);
  }
  g_app.slots[slot_id];
  return slot_id;
}

void AsyncShutdownRendererAndWindow(std::shared_ptr<vkwlf::VkRenderer> old_renderer,
                                    ANativeWindow* old_window) {
  if (!old_renderer && !old_window) return;
  std::thread([old_renderer = std::move(old_renderer), old_window]() mutable {
    if (old_renderer) {
      old_renderer->Shutdown();
      old_renderer.reset();
    }
#ifdef __ANDROID__
    if (old_window) {
      ANativeWindow_release(old_window);
    }
#else
    (void)old_window;
#endif
  }).detach();
}

void ReleaseWindowSlot(int slot_idx) {
  if (slot_idx < 0) return;
  std::shared_ptr<vkwlf::VkRenderer> old_renderer;
  ANativeWindow* old_window = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_app.slots_mutex);
    auto it = g_app.slots.find(slot_idx);
    if (it == g_app.slots.end()) return;
    WindowSlot& slot = it->second;
    ++slot.window_seq;
    old_renderer = std::move(slot.renderer);
    old_window = slot.window;
    slot.window = nullptr;
#ifdef __ANDROID__
    if (g_app.jvm && slot.activity_global) {
      JNIEnv* env = nullptr;
      if (g_app.jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env) {
        env->DeleteGlobalRef(slot.activity_global);
      }
      slot.activity_global = nullptr;
    }
#endif
    g_app.slots.erase(it);
  }
  AsyncShutdownRendererAndWindow(std::move(old_renderer), old_window);
}

void AttachWindowSlot(int slot_idx,
                      void* activity_thiz,
                      ANativeWindow* window,
                      uint32_t width,
                      uint32_t height) {
  if (slot_idx < 0) return;
  EnsureServerAndPumpStarted("/data/local/tmp/wayland-0", 5555, 5555, width, height);

  std::shared_ptr<vkwlf::VkRenderer> old_renderer;
  ANativeWindow* old_window = nullptr;
  uint64_t seq = 0;
  uint32_t init_w = width > 0 ? width : 1280;
  uint32_t init_h = height > 0 ? height : 720;

  {
    std::lock_guard<std::mutex> lock(g_app.slots_mutex);
    WindowSlot& slot = g_app.slots[slot_idx];
#ifdef __ANDROID__
    if (g_app.jvm && activity_thiz) {
      JNIEnv* env = nullptr;
      if (g_app.jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env) {
        if (slot.activity_global) {
          env->DeleteGlobalRef(slot.activity_global);
        }
        slot.activity_global = env->NewGlobalRef(reinterpret_cast<jobject>(activity_thiz));
        slot.last_notified_app_key.clear();
        slot.last_notified_has_icon = false;
      }
    }
#else
    (void)activity_thiz;
#endif
    old_renderer = std::move(slot.renderer);
    if (slot.window && slot.window != window) {
      old_window = slot.window;
    }
#ifdef __ANDROID__
    if (window) {
      ANativeWindow_acquire(window);  // Reference owned by slot.window
      ANativeWindow_acquire(window);  // Reference owned by init worker thread
    }
#endif
    slot.window = window;
    slot.width = init_w;
    slot.height = init_h;
    seq = ++slot.window_seq;
  }

  UpdateSlotAssignmentsAndNotify();
  if (g_app.server) {
    auto catalog = g_app.server->GetGuestAppCatalog();
    if (!catalog.empty()) {
      NotifyAllActivitiesGuestCatalog(catalog);
    }
  }

  uint32_t target_sid = 0;
  {
    std::lock_guard<std::mutex> lock(g_app.slots_mutex);
    auto it = g_app.slots.find(slot_idx);
    if (it != g_app.slots.end()) {
      target_sid = it->second.target_surface_id;
    }
  }
  if (g_app.server) {
    if (target_sid != 0) {
      g_app.server->SetActiveInputSurface(target_sid, init_w, init_h);
      g_app.server->SetDisplaySizeForSurface(target_sid, init_w, init_h);
    } else {
      g_app.server->SetDisplaySize(init_w, init_h);
    }
  }

  std::thread([slot_idx, window, seq, init_w, init_h, old_renderer = std::move(old_renderer),
               old_window]() mutable {
    if (old_renderer) {
      old_renderer->Shutdown();
      old_renderer.reset();
    }
#ifdef __ANDROID__
    if (old_window) {
      ANativeWindow_release(old_window);
      old_window = nullptr;
    }
#endif

    uint32_t cur_w = init_w;
    uint32_t cur_h = init_h;
    uint32_t cur_target_sid = 0;
    {
      std::lock_guard<std::mutex> lock(g_app.slots_mutex);
      auto it = g_app.slots.find(slot_idx);
      if (it == g_app.slots.end() || it->second.window_seq != seq || it->second.window != window) {
#ifdef __ANDROID__
        if (window) ANativeWindow_release(window);
#endif
        return;
      }
      cur_w = it->second.width;
      cur_h = it->second.height;
      cur_target_sid = it->second.target_surface_id;
    }

    std::vector<vkwlf::CommittedFrame> replay_frames;
    {
      std::lock_guard<std::mutex> lock(g_app.cache_mutex);
      replay_frames = CollectReplayFramesForTargetLocked(cur_target_sid);
    }

    vkwlf::VkRenderer::Config vk_cfg;
    vk_cfg.width = cur_w;
    vk_cfg.height = cur_h;
    auto renderer = std::make_shared<vkwlf::VkRenderer>(vk_cfg);
    if (!renderer->Initialize(window)) {
      LOGE(kTag, "Failed to initialize VkRenderer for WindowSlot %d", slot_idx);
      for (auto& rf : replay_frames) {
        CloseFrameFds(rf);
      }
#ifdef __ANDROID__
      if (window) ANativeWindow_release(window);
#endif
      return;
    }

    renderer->SetTargetSurfaceId(cur_target_sid);
    if (!replay_frames.empty()) {
      for (const auto& rf : replay_frames) {
        renderer->ProcessAndPresentFrame(rf);
      }
    } else {
      renderer->Repaint();
    }

#ifdef __ANDROID__
    if (g_app.jvm) {
      JNIEnv* jni_env = nullptr;
      bool attached = false;
      if (g_app.jvm->GetEnv(reinterpret_cast<void**>(&jni_env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_app.jvm->AttachCurrentThread(&jni_env, nullptr) == JNI_OK) {
          attached = true;
        }
      }
      if (jni_env) {
        jobject act_ref = nullptr;
        {
          std::lock_guard<std::mutex> lock(g_app.slots_mutex);
          auto it = g_app.slots.find(slot_idx);
          if (it != g_app.slots.end() && it->second.window_seq == seq &&
              it->second.activity_global) {
            act_ref = jni_env->NewLocalRef(it->second.activity_global);
          }
        }
        if (act_ref) {
          jclass act_cls = jni_env->GetObjectClass(act_ref);
          if (act_cls) {
            jmethodID mid = jni_env->GetMethodID(act_cls, "onFirstVulkanFramePresented", "()V");
            if (mid) {
              jni_env->CallVoidMethod(act_ref, mid);
            } else {
              jni_env->ExceptionClear();
            }
            jni_env->DeleteLocalRef(act_cls);
          }
          jni_env->DeleteLocalRef(act_ref);
        }
        if (attached) {
          g_app.jvm->DetachCurrentThread();
        }
      }
    }
#endif

    uint32_t post_w = cur_w;
    uint32_t post_h = cur_h;
    uint32_t post_target_sid = cur_target_sid;
    bool attached_ok = false;
    {
      std::lock_guard<std::mutex> lock(g_app.slots_mutex);
      auto it = g_app.slots.find(slot_idx);
      if (it != g_app.slots.end() && it->second.window_seq == seq && it->second.window == window) {
        post_w = it->second.width;
        post_h = it->second.height;
        post_target_sid = it->second.target_surface_id;
        it->second.renderer = renderer;
        attached_ok = true;
      }
    }

    if (!attached_ok) {
      renderer->Shutdown();
      renderer.reset();
    } else {
      if (post_target_sid != cur_target_sid) {
        renderer->SetTargetSurfaceId(post_target_sid);
        std::vector<vkwlf::CommittedFrame> post_frames;
        {
          std::lock_guard<std::mutex> lock(g_app.cache_mutex);
          post_frames = CollectReplayFramesForTargetLocked(post_target_sid);
        }
        for (const auto& pf : post_frames) {
          renderer->ProcessAndPresentFrame(pf);
        }
      }
      if (post_w != cur_w || post_h != cur_h) {
        renderer->Resize(post_w, post_h);
      }
    }
#ifdef __ANDROID__
    if (window) ANativeWindow_release(window);
#endif
  }).detach();
}

void ResizeWindowSlot(int slot_idx, uint32_t width, uint32_t height) {
  if (slot_idx < 0 || width == 0 || height == 0) return;
  uint32_t target_sid = 0;
  std::shared_ptr<vkwlf::VkRenderer> renderer;
  bool size_changed = false;
  {
    std::lock_guard<std::mutex> lock(g_app.slots_mutex);
    auto it = g_app.slots.find(slot_idx);
    if (it == g_app.slots.end()) return;
    WindowSlot& slot = it->second;
    size_changed = (slot.width != width || slot.height != height);
    slot.width = width;
    slot.height = height;
    target_sid = slot.target_surface_id;
    renderer = slot.renderer;
  }
  if (size_changed && renderer) {
    std::thread([renderer = std::move(renderer), width, height]() mutable {
      renderer->Resize(width, height);
    }).detach();
  }
  if (size_changed && g_app.server) {
    if (target_sid != 0) {
      g_app.server->SetActiveInputSurface(target_sid, width, height);
      g_app.server->SetDisplaySizeForSurface(target_sid, width, height);
    } else {
      g_app.server->SetDisplaySize(width, height);
    }
  }
}

void DetachWindowSlot(int slot_idx) {
  if (slot_idx < 0) return;
  std::shared_ptr<vkwlf::VkRenderer> old_renderer;
  ANativeWindow* old_window = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_app.slots_mutex);
    auto it = g_app.slots.find(slot_idx);
    if (it == g_app.slots.end()) return;
    WindowSlot& slot = it->second;
    ++slot.window_seq;
    old_renderer = std::move(slot.renderer);
    old_window = slot.window;
    slot.window = nullptr;
  }
  AsyncShutdownRendererAndWindow(std::move(old_renderer), old_window);
}

void SelectSurfaceForWindowSlot(int slot_idx, uint32_t surface_id) {
  if (slot_idx < 0 || surface_id == 0) return;
  std::string title;
  std::string app_id;
  std::vector<uint8_t> icon_png;
  std::vector<vkwlf::CommittedFrame> target_frames;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(g_app.cache_mutex);
    auto it = g_app.cached_frames.find(surface_id);
    if (it != g_app.cached_frames.end()) {
      title = it->second.title;
      app_id = it->second.app_id;
      icon_png = it->second.icon_png;
      found = true;
    }
    target_frames = CollectReplayFramesForTargetLocked(surface_id);
  }
  uint32_t slot_w = 0;
  uint32_t slot_h = 0;
  bool has_window = false;
  std::shared_ptr<vkwlf::VkRenderer> renderer;
  {
    std::lock_guard<std::mutex> lock(g_app.slots_mutex);
    WindowSlot& slot = g_app.slots[slot_idx];
    slot.target_surface_id = surface_id;
    slot.user_pinned_surface = true;
    slot_w = slot.width;
    slot_h = slot.height;
    has_window = (slot.window != nullptr);
    renderer = slot.renderer;
  }
  if (renderer) {
    std::thread([renderer = std::move(renderer), surface_id,
                 frames = std::move(target_frames)]() mutable {
      renderer->SetTargetSurfaceId(surface_id);
      if (!frames.empty()) {
        for (const auto& f : frames) {
          renderer->ProcessAndPresentFrame(f);
        }
      } else {
        renderer->Repaint();
      }
    }).detach();
  } else {
    for (auto& f : target_frames) {
      CloseFrameFds(f);
    }
  }
  if (g_app.server) {
    g_app.server->SetActiveInputSurface(surface_id);
    if (has_window && slot_w > 0 && slot_h > 0) {
      g_app.server->SetDisplaySizeForSurface(surface_id, slot_w, slot_h);
    }
  }
  if (found) {
    NotifySlotAppInfo(slot_idx, surface_id, title, app_id, icon_png, true);
  }
  NotifyAllActivitiesAppsList();
}

}  // namespace

#ifdef __ANDROID__
extern "C" {

// --- Standalone VkWlfApp (com.google.example.vkwlf.MainActivity) JNI bindings ---

JNIEXPORT jint JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeAllocateWindowSlot(JNIEnv* env,
                                                                    jclass /*clazz*/,
                                                                    jint preferred_slot) {
  env->GetJavaVM(&g_app.jvm);
  int slot_id = AllocateWindowSlot(static_cast<int>(preferred_slot));
  LOGI(kTag, "JNI nativeAllocateWindowSlot(preferred=%d) -> slot=%d", preferred_slot, slot_id);
  return static_cast<jint>(slot_id);
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeReleaseWindowSlot(JNIEnv* /*env*/,
                                                                   jclass /*clazz*/,
                                                                   jint slot) {
  LOGI(kTag, "JNI nativeReleaseWindowSlot(slot=%d)", slot);
  ReleaseWindowSlot(static_cast<int>(slot));
}

JNIEXPORT jbyteArray JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeGetSurfaceIconPng(JNIEnv* env,
                                                                   jclass /*clazz*/,
                                                                   jint surface_id) {
  std::vector<uint8_t> icon_png;
  {
    std::lock_guard<std::mutex> lock(g_app.cache_mutex);
    auto it = g_app.cached_frames.find(static_cast<uint32_t>(surface_id));
    if (it != g_app.cached_frames.end()) {
      icon_png = it->second.icon_png;
    }
  }
  if (icon_png.empty()) return nullptr;
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(icon_png.size()));
  if (arr) {
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(icon_png.size()),
                            reinterpret_cast<const jbyte*>(icon_png.data()));
  }
  return arr;
}

JNIEXPORT jbyteArray JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeGetCatalogIconPng(JNIEnv* env,
                                                                   jclass /*clazz*/,
                                                                   jstring j_desktop_id) {
  if (!j_desktop_id || !g_app.server) return nullptr;
  const char* cid = env->GetStringUTFChars(j_desktop_id, nullptr);
  std::string target_id = cid ? cid : "";
  if (cid) env->ReleaseStringUTFChars(j_desktop_id, cid);
  if (target_id.empty()) return nullptr;

  std::vector<uint8_t> icon_png;
  auto catalog = g_app.server->GetGuestAppCatalog();
  for (const auto& entry : catalog) {
    if (entry.desktop_id == target_id && !entry.icon_png.empty()) {
      icon_png = entry.icon_png;
      break;
    }
  }
  if (icon_png.empty()) return nullptr;
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(icon_png.size()));
  if (arr) {
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(icon_png.size()),
                            reinterpret_cast<const jbyte*>(icon_png.data()));
  }
  return arr;
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeLaunchGuestApp(JNIEnv* env,
                                                                jclass /*clazz*/,
                                                                jstring j_launch_spec) {
  env->GetJavaVM(&g_app.jvm);
  EnsureServerAndPumpStarted("/data/local/tmp/wayland-0", 5555, 5555, 1280, 720);
  if (!j_launch_spec || !g_app.server) return;
  const char* cspec = env->GetStringUTFChars(j_launch_spec, nullptr);
  std::string spec = cspec ? cspec : "";
  if (cspec) env->ReleaseStringUTFChars(j_launch_spec, cspec);
  if (!spec.empty()) {
    LOGI(kTag, "JNI nativeLaunchGuestApp('%s')", spec.c_str());
    g_app.server->RequestGuestAppLaunch(spec);
  }
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeRefreshGuestAppCatalog(JNIEnv* env,
                                                                        jclass /*clazz*/) {
  env->GetJavaVM(&g_app.jvm);
  EnsureServerAndPumpStarted("/data/local/tmp/wayland-0", 5555, 5555, 1280, 720);
  if (g_app.server) {
    auto cached = g_app.server->GetGuestAppCatalog();
    if (!cached.empty()) {
      NotifyAllActivitiesGuestCatalog(cached);
    }
    g_app.server->RequestGuestAppLaunch("__REFRESH_CATALOG__");
  }
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceCreatedForSlot(JNIEnv* env,
                                                                         jobject thiz,
                                                                         jint slot,
                                                                         jobject surface,
                                                                         jint width,
                                                                         jint height) {
  env->GetJavaVM(&g_app.jvm);
  ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
  LOGI(kTag, "JNI nativeOnSurfaceCreatedForSlot(slot=%d): ANativeWindow=%p (%dx%d)", slot,
       static_cast<void*>(win), width, height);
  AttachWindowSlot(static_cast<int>(slot), thiz, win,
                   static_cast<uint32_t>(width > 0 ? width : 1280),
                   static_cast<uint32_t>(height > 0 ? height : 720));
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceChangedForSlot(JNIEnv* /*env*/,
                                                                         jobject /*thiz*/,
                                                                         jint slot,
                                                                         jint width,
                                                                         jint height) {
  LOGI(kTag, "JNI nativeOnSurfaceChangedForSlot(slot=%d): %dx%d", slot, width, height);
  if (width > 0 && height > 0) {
    ResizeWindowSlot(static_cast<int>(slot), static_cast<uint32_t>(width),
                     static_cast<uint32_t>(height));
  }
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceDestroyedForSlot(JNIEnv* /*env*/,
                                                                           jobject /*thiz*/,
                                                                           jint slot) {
  LOGI(kTag, "JNI nativeOnSurfaceDestroyedForSlot(slot=%d)", slot);
  DetachWindowSlot(static_cast<int>(slot));
}

JNIEXPORT jboolean JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnMotionEventForSlot(JNIEnv* /*env*/,
                                                                      jobject /*thiz*/,
                                                                      jint slot,
                                                                      jint action,
                                                                      jfloat x,
                                                                      jfloat y,
                                                                      jint button_state,
                                                                      jfloat vscroll,
                                                                      jfloat hscroll,
                                                                      jlong event_time_ms) {
  if (!g_app.input) return JNI_FALSE;
  int slot_idx = static_cast<int>(slot);
  float mapped_x = x;
  float mapped_y = y;
  if (slot_idx >= 0) {
    uint32_t target_sid = 0;
    uint32_t slot_w = 0;
    uint32_t slot_h = 0;
    {
      std::lock_guard<std::mutex> lock(g_app.slots_mutex);
      auto sit = g_app.slots.find(slot_idx);
      if (sit != g_app.slots.end()) {
        target_sid = sit->second.target_surface_id;
        slot_w = sit->second.width;
        slot_h = sit->second.height;
      }
    }
    if (target_sid != 0) {
      if (g_app.server) {
        g_app.server->SetActiveInputSurface(target_sid);
      }
      uint32_t surf_w = 0;
      uint32_t surf_h = 0;
      {
        std::lock_guard<std::mutex> lock(g_app.cache_mutex);
        auto it = g_app.cached_frames.find(target_sid);
        if (it != g_app.cached_frames.end()) {
          surf_w = it->second.width;
          surf_h = it->second.height;
        }
      }
      if (slot_w > 0 && slot_h > 0 && surf_w > 0 && surf_h > 0 &&
          (surf_w != slot_w || surf_h != slot_h)) {
        mapped_x = x * static_cast<float>(surf_w) / static_cast<float>(slot_w);
        mapped_y = y * static_cast<float>(surf_h) / static_cast<float>(slot_h);
      }
    }
  }
  bool handled = g_app.input->OnAndroidMotionEvent(
      action, mapped_x, mapped_y, button_state, vscroll, hscroll,
      static_cast<uint32_t>(event_time_ms));
  return handled ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnKeyEventForSlot(JNIEnv* /*env*/,
                                                                   jobject /*thiz*/,
                                                                   jint slot,
                                                                   jint action,
                                                                   jint keycode,
                                                                   jint meta_state,
                                                                   jlong event_time_ms) {
  if (!g_app.input) return JNI_FALSE;
  int slot_idx = static_cast<int>(slot);
  if (slot_idx >= 0 && g_app.server) {
    uint32_t target_sid = 0;
    {
      std::lock_guard<std::mutex> lock(g_app.slots_mutex);
      auto sit = g_app.slots.find(slot_idx);
      if (sit != g_app.slots.end()) {
        target_sid = sit->second.target_surface_id;
      }
    }
    if (target_sid != 0) {
      g_app.server->SetActiveInputSurface(target_sid);
    }
  }
  bool handled = g_app.input->OnAndroidKeyEvent(action, keycode, meta_state,
                                                static_cast<uint32_t>(event_time_ms));
  return handled ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeSelectSurfaceForSlot(JNIEnv* /*env*/,
                                                                      jobject /*thiz*/,
                                                                      jint slot,
                                                                      jint surface_id) {
  LOGI(kTag, "JNI nativeSelectSurfaceForSlot(slot=%d, surface_id=%d)", slot, surface_id);
  SelectSurfaceForWindowSlot(static_cast<int>(slot), static_cast<uint32_t>(surface_id));
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceCreated(JNIEnv* env,
                                                                  jobject thiz,
                                                                  jobject surface,
                                                                  jint width,
                                                                  jint height) {
  Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceCreatedForSlot(env, thiz, 0, surface,
                                                                           width, height);
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceChanged(JNIEnv* env,
                                                                  jobject thiz,
                                                                  jint width,
                                                                  jint height) {
  Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceChangedForSlot(env, thiz, 0, width,
                                                                           height);
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceDestroyed(JNIEnv* env, jobject thiz) {
  Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceDestroyedForSlot(env, thiz, 0);
}

JNIEXPORT jboolean JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnMotionEvent(JNIEnv* env,
                                                               jobject thiz,
                                                               jint action,
                                                               jfloat x,
                                                               jfloat y,
                                                               jint button_state,
                                                               jfloat vscroll,
                                                               jfloat hscroll,
                                                               jlong event_time_ms) {
  return Java_com_google_example_vkwlf_MainActivity_nativeOnMotionEventForSlot(
      env, thiz, 0, action, x, y, button_state, vscroll, hscroll, event_time_ms);
}

JNIEXPORT jboolean JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnKeyEvent(JNIEnv* env,
                                                            jobject thiz,
                                                            jint action,
                                                            jint keycode,
                                                            jint meta_state,
                                                            jlong event_time_ms) {
  return Java_com_google_example_vkwlf_MainActivity_nativeOnKeyEventForSlot(
      env, thiz, 0, action, keycode, meta_state, event_time_ms);
}

// --- Android Terminal app (com.android.virtualization.terminal.VkWlfCompositorBridge) JNI bindings ---

JNIEXPORT void JNICALL
Java_com_android_virtualization_terminal_VkWlfCompositorBridge_nativeStartServer(
    JNIEnv* env,
    jclass /*clazz*/,
    jstring j_socket_path,
    jint vsock_port,
    jint tcp_port,
    jint width,
    jint height,
    jobject listener) {
  env->GetJavaVM(&g_app.jvm);
  if (g_app.app_launch_listener_global) {
    env->DeleteGlobalRef(g_app.app_launch_listener_global);
    g_app.app_launch_listener_global = nullptr;
  }
  if (listener) {
    g_app.app_launch_listener_global = env->NewGlobalRef(listener);
  }
  const char* c_sock = j_socket_path ? env->GetStringUTFChars(j_socket_path, nullptr) : nullptr;
  std::string sock_path = c_sock ? c_sock : "/data/local/tmp/wayland-0";
  if (c_sock) env->ReleaseStringUTFChars(j_socket_path, c_sock);

  EnsureServerAndPumpStarted(sock_path, vsock_port, tcp_port,
                             static_cast<uint32_t>(width > 0 ? width : 1920),
                             static_cast<uint32_t>(height > 0 ? height : 1080));
}

JNIEXPORT void JNICALL
Java_com_android_virtualization_terminal_VkWlfCompositorBridge_nativeOnSurfaceCreated(
    JNIEnv* env, jclass clazz, jobject surface, jint width, jint height) {
  Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceCreated(env, clazz, surface, width,
                                                                    height);
}

JNIEXPORT void JNICALL
Java_com_android_virtualization_terminal_VkWlfCompositorBridge_nativeOnSurfaceChanged(
    JNIEnv* env, jclass clazz, jint width, jint height) {
  Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceChanged(env, clazz, width, height);
}

JNIEXPORT void JNICALL
Java_com_android_virtualization_terminal_VkWlfCompositorBridge_nativeOnSurfaceDestroyed(
    JNIEnv* env, jclass clazz) {
  Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceDestroyed(env, clazz);
}

JNIEXPORT jboolean JNICALL
Java_com_android_virtualization_terminal_VkWlfCompositorBridge_nativeOnMotionEvent(
    JNIEnv* env,
    jclass clazz,
    jint action,
    jfloat x,
    jfloat y,
    jint button_state,
    jfloat vscroll,
    jfloat hscroll,
    jlong event_time_ms) {
  return Java_com_google_example_vkwlf_MainActivity_nativeOnMotionEvent(
      env, clazz, action, x, y, button_state, vscroll, hscroll, event_time_ms);
}

JNIEXPORT jboolean JNICALL
Java_com_android_virtualization_terminal_VkWlfCompositorBridge_nativeOnKeyEvent(
    JNIEnv* env, jclass clazz, jint action, jint keycode, jint meta_state, jlong event_time_ms) {
  return Java_com_google_example_vkwlf_MainActivity_nativeOnKeyEvent(env, clazz, action, keycode,
                                                                     meta_state, event_time_ms);
}

}  // extern "C"
#endif

int main(int argc, char** argv) {
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  std::string socket_path = "/data/local/tmp/wayland-0";
  int vsock_port = 5555;
  int tcp_port = 5555;
  uint32_t width = 1280;
  uint32_t height = 720;
  uint64_t max_frames = 0;
  uint32_t target_surface = 0;
  bool inject_test_input = false;
  std::string dump_ppm_path;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (strcmp(argv[i], "--vsock-port") == 0 && i + 1 < argc) {
      vsock_port = std::atoi(argv[++i]);
    } else if (strcmp(argv[i], "--tcp-port") == 0 && i + 1 < argc) {
      tcp_port = std::atoi(argv[++i]);
    } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
      width = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
      height = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc) {
      max_frames = static_cast<uint64_t>(std::atoll(argv[++i]));
    } else if (strcmp(argv[i], "--target-surface") == 0 && i + 1 < argc) {
      target_surface = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (strcmp(argv[i], "--inject-test-input") == 0) {
      inject_test_input = true;
    } else if (strcmp(argv[i], "--dump-ppm") == 0 && i + 1 < argc) {
      dump_ppm_path = argv[++i];
    }
  }

  LOGI(kTag, "Starting VkWlfApp In-Process Wayland-to-Vulkan Compositor (%ux%u)", width, height);

  vkwlf::WaylandServerEngine::Config srv_cfg;
  srv_cfg.socket_path = socket_path;
  srv_cfg.vsock_port = vsock_port;
  srv_cfg.tcp_port = tcp_port;
  srv_cfg.display_width = width;
  srv_cfg.display_height = height;

  vkwlf::WaylandServerEngine server(srv_cfg);
  server.SetAppLaunchedCallback(
      [](uint32_t surface_id, const std::string& title, const std::string& app_id,
         const std::vector<uint8_t>& icon_png) {
        LOGI(kTag,
             "[AutoForeground] Guest Wayland application launched: surface=%u title='%s' "
             "app_id='%s' icon_bytes=%zu",
             surface_id, title.c_str(), app_id.c_str(), icon_png.size());
      });

  if (!server.Initialize() || !server.Start()) {
    LOGE(kTag, "Failed to start WaylandServerEngine");
    return 1;
  }

  vkwlf::VkRenderer::Config vk_cfg;
  vk_cfg.width = width;
  vk_cfg.height = height;
  vkwlf::VkRenderer renderer(vk_cfg);
  if (!renderer.Initialize(nullptr)) {
    LOGE(kTag, "Failed to initialize VkRenderer");
    server.Stop();
    return 1;
  }
  if (target_surface != 0) {
    renderer.SetTargetSurfaceId(target_surface);
  }

  vkwlf::InputHandler input_handler(&server);
  bool input_injected = false;

  while (g_keep_running.load()) {
    vkwlf::CommittedFrame frame;
    if (server.PopCommittedFrame(&frame, 20)) {
      if (!renderer.ProcessAndPresentFrame(frame)) {
        LOGE(kTag, "Failed to process and present frame %llu",
             static_cast<unsigned long long>(frame.frame_number));
      }

      if (inject_test_input && !input_injected && frame.frame_number >= 2) {
        input_injected = true;
        // Simulate Android NDK touch/mouse motion, left click, and key press ('V' for VSCode).
        input_handler.OnAndroidMotionEvent(2 /*ACTION_MOVE*/, 320.0f, 240.0f, 0, 0.0f, 0.0f, 100);
        input_handler.OnAndroidMotionEvent(0 /*ACTION_DOWN*/, 320.0f, 240.0f, 1 /*PRIMARY*/, 0.0f,
                                           0.0f, 110);
        input_handler.OnAndroidMotionEvent(1 /*ACTION_UP*/, 320.0f, 240.0f, 0, 0.0f, 0.0f, 120);
        input_handler.OnAndroidKeyEvent(0 /*ACTION_DOWN*/, 50 /*AKEYCODE_V*/, 0x01 /*SHIFT*/, 130);
        input_handler.OnAndroidKeyEvent(1 /*ACTION_UP*/, 50 /*AKEYCODE_V*/, 0, 140);
      }

      auto stats = renderer.GetStats();
      if (max_frames > 0 && stats.frames_rendered >= max_frames) {
        LOGI(kTag,
             "Reached target max_frames=%llu (shm_uploads=%llu, dmabuf_imports=%llu, "
             "presents=%llu, lastPixel=0x%08x)",
             static_cast<unsigned long long>(max_frames),
             static_cast<unsigned long long>(stats.shm_uploads),
             static_cast<unsigned long long>(stats.dmabuf_imports),
             static_cast<unsigned long long>(stats.presents_succeeded),
             stats.last_sampled_pixel_rgba);
        break;
      }
    }
  }

  if (!dump_ppm_path.empty()) {
    renderer.SaveLastFramePpm(dump_ppm_path);
  }
  renderer.Shutdown();
  server.Stop();
  LOGI(kTag, "VkWlfApp compositor shutdown cleanly");
  return 0;
}
