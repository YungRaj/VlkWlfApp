#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

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

struct AppBridgeState {
  std::unique_ptr<vkwlf::WaylandServerEngine> server;
  std::mutex renderer_mutex;
  std::unique_ptr<vkwlf::VkRenderer> renderer;
  std::unique_ptr<vkwlf::InputHandler> input;
  std::thread render_thread;
  std::atomic<bool> render_running{false};
  std::atomic<uint32_t> latest_width{0};
  std::atomic<uint32_t> latest_height{0};
  ANativeWindow* window = nullptr;
  std::mutex cache_mutex;
  std::unordered_map<uint32_t, vkwlf::CommittedFrame> cached_frames;
#ifdef __ANDROID__
  JavaVM* jvm = nullptr;
  jobject app_launch_listener_global = nullptr;
  jobject main_activity_global = nullptr;
  std::string last_overlay_app_id;
  bool overlay_icon_sent = false;
#endif
};

AppBridgeState g_app;

void NotifyAndroidAppInfo(uint32_t surface_id,
                          const std::string& title,
                          const std::string& app_id,
                          const std::vector<uint8_t>& icon_png) {
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

  // Only invoke MainActivity.onWaylandAppInfo once per app_id (or when its icon first resolves)
  // so OverlayManager.commit(FabricatedOverlay) does not repeatedly reload SystemUI resources.
  const std::string effective_app_id = !app_id.empty() ? app_id : title;
  const bool need_activity_update =
      (effective_app_id != g_app.last_overlay_app_id) ||
      (!g_app.overlay_icon_sent && !icon_png.empty());
  if (g_app.main_activity_global && need_activity_update) {
    g_app.last_overlay_app_id = effective_app_id;
    if (!icon_png.empty()) {
      g_app.overlay_icon_sent = true;
    }
    jclass act_cls = env->GetObjectClass(g_app.main_activity_global);
    if (act_cls) {
      jmethodID mid = env->GetMethodID(
          act_cls, "onWaylandAppInfo", "(ILjava/lang/String;Ljava/lang/String;[B)V");
      if (mid) {
        env->CallVoidMethod(g_app.main_activity_global, mid, static_cast<jint>(surface_id),
                            j_title, j_app_id, j_icon);
      } else {
        env->ExceptionClear();
      }
      env->DeleteLocalRef(act_cls);
    }
  }

  if (g_app.app_launch_listener_global) {
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

void EnsureServerStarted(const std::string& socket_path,
                         int vsock_port,
                         int tcp_port,
                         uint32_t width,
                         uint32_t height) {
  if (g_app.server) {
    g_app.server->SetDisplaySize(width, height);
    return;
  }

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
        NotifyAndroidAppInfo(surface_id, title, app_id, icon_png);
      });

  if (g_app.server->Initialize()) {
    g_app.server->Start();
  }
  g_app.input = std::make_unique<vkwlf::InputHandler>(g_app.server.get());
}

void StopRenderLoop();

void StartRenderLoop(ANativeWindow* window, uint32_t width, uint32_t height) {
  StopRenderLoop();

  if (width > 0 && height > 0) {
    g_app.latest_width.store(width);
    g_app.latest_height.store(height);
  }
  EnsureServerStarted("/data/local/tmp/wayland-0", 5555, 5555, width, height);

  g_app.window = window;
  g_app.render_running.store(true);
  g_app.render_thread = std::thread([window, width, height]() {
    vkwlf::VkRenderer::Config vk_cfg;
    vk_cfg.width = g_app.latest_width.load() > 0 ? g_app.latest_width.load() : width;
    vk_cfg.height = g_app.latest_height.load() > 0 ? g_app.latest_height.load() : height;
    auto local_renderer = std::make_unique<vkwlf::VkRenderer>(vk_cfg);
    if (!local_renderer->Initialize(window)) {
      LOGE(kTag, "Failed to initialize VkRenderer on render thread");
      return;
    }
    uint32_t final_w = g_app.latest_width.load();
    uint32_t final_h = g_app.latest_height.load();
    if (final_w == 0 || final_h == 0) {
      final_w = width;
      final_h = height;
    }
    {
      std::lock_guard<std::mutex> lock(g_app.renderer_mutex);
      local_renderer->Resize(final_w, final_h);
      g_app.renderer = std::move(local_renderer);
    }
    if (g_app.server) {
      g_app.server->SetDisplaySize(final_w, final_h);
    }

    // Immediately replay any cached surface frames if SurfaceView was recreated
    {
      std::vector<vkwlf::CommittedFrame> replay_frames;
      {
        std::lock_guard<std::mutex> lock(g_app.cache_mutex);
        for (const auto& [sid, cached] : g_app.cached_frames) {
          if (!cached.shm_pixels.empty()) {
            replay_frames.push_back(cached);
          }
        }
      }
      std::sort(replay_frames.begin(), replay_frames.end(),
                [](const vkwlf::CommittedFrame& a, const vkwlf::CommittedFrame& b) {
                  return a.surface_id < b.surface_id;
                });
      std::lock_guard<std::mutex> lock(g_app.renderer_mutex);
      if (g_app.renderer) {
        for (const auto& rf : replay_frames) {
          if (!rf.is_popup && !rf.is_subsurface) {
            NotifyAndroidAppInfo(rf.surface_id, rf.title, rf.app_id, rf.icon_png);
          }
          g_app.renderer->ProcessAndPresentFrame(rf);
        }
      }
    }

    while (g_app.render_running.load()) {
      vkwlf::CommittedFrame frame;
      if (g_app.server->PopCommittedFrame(&frame, 16)) {
        {
          std::lock_guard<std::mutex> lock(g_app.cache_mutex);
          if (frame.is_unmapped) {
            g_app.cached_frames.erase(frame.surface_id);
          } else if (!frame.shm_pixels.empty()) {
            // Preserve previously received icon_png across subsequent frames of the same surface
            if (frame.icon_png.empty()) {
              auto it = g_app.cached_frames.find(frame.surface_id);
              if (it != g_app.cached_frames.end() && !it->second.icon_png.empty()) {
                frame.icon_png = it->second.icon_png;
              }
            }
            g_app.cached_frames[frame.surface_id] = frame;
          }
        }
        std::lock_guard<std::mutex> lock(g_app.renderer_mutex);
        if (g_app.renderer) {
          g_app.renderer->ProcessAndPresentFrame(frame);
          auto stats = g_app.renderer->GetStats();
          if (stats.presents_succeeded <= 5 || (stats.presents_succeeded % 60 == 0)) {
            LOGI(kTag,
                 "[HostGPU] Presented Wayland frame #%llu (surface=%u, buffer_type=%s, "
                 "shm_uploads=%llu, dmabuf_imports=%llu, presents=%llu)",
                 static_cast<unsigned long long>(frame.frame_number), frame.surface_id,
                 frame.buffer_type == vkwlf::BufferType::kDmaBuf ? "DMA-BUF/AHB" : "SHM",
                 static_cast<unsigned long long>(stats.shm_uploads),
                 static_cast<unsigned long long>(stats.dmabuf_imports),
                 static_cast<unsigned long long>(stats.presents_succeeded));
          }
        }
      }
    }
    std::lock_guard<std::mutex> lock(g_app.renderer_mutex);
    if (g_app.renderer) {
      g_app.renderer->Shutdown();
      g_app.renderer.reset();
    }
  });
}

void StopRenderLoop() {
  if (g_app.render_running.exchange(false)) {
    if (g_app.render_thread.joinable()) {
      g_app.render_thread.join();
    }
  }
#ifdef __ANDROID__
  if (g_app.window) {
    ANativeWindow_release(g_app.window);
    g_app.window = nullptr;
  }
#endif
}

}  // namespace

#ifdef __ANDROID__
extern "C" {

// --- Standalone VkWlfApp (com.google.example.vkwlf.MainActivity) JNI bindings ---

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceCreated(JNIEnv* env,
                                                                  jobject thiz,
                                                                  jobject surface,
                                                                  jint width,
                                                                  jint height) {
  env->GetJavaVM(&g_app.jvm);
  if (g_app.main_activity_global) {
    env->DeleteGlobalRef(g_app.main_activity_global);
    g_app.main_activity_global = nullptr;
  }
  if (thiz) {
    g_app.main_activity_global = env->NewGlobalRef(thiz);
  }
  ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
  LOGI(kTag, "JNI nativeOnSurfaceCreated: ANativeWindow=%p (%dx%d)", static_cast<void*>(win), width,
       height);
  StartRenderLoop(win, static_cast<uint32_t>(width > 0 ? width : 1280),
                  static_cast<uint32_t>(height > 0 ? height : 720));
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceChanged(JNIEnv* /*env*/,
                                                                  jobject /*thiz*/,
                                                                  jint width,
                                                                  jint height) {
  LOGI(kTag, "JNI nativeOnSurfaceChanged: %dx%d", width, height);
  if (width > 0 && height > 0) {
    g_app.latest_width.store(static_cast<uint32_t>(width));
    g_app.latest_height.store(static_cast<uint32_t>(height));
  }
  if (g_app.server) {
    g_app.server->SetDisplaySize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
  }
  std::lock_guard<std::mutex> lock(g_app.renderer_mutex);
  if (g_app.renderer) {
    g_app.renderer->Resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
  }
}

JNIEXPORT void JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnSurfaceDestroyed(JNIEnv* /*env*/,
                                                                    jobject /*thiz*/) {
  LOGI(kTag, "JNI nativeOnSurfaceDestroyed");
  StopRenderLoop();
}

JNIEXPORT jboolean JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnMotionEvent(JNIEnv* /*env*/,
                                                               jobject /*thiz*/,
                                                               jint action,
                                                               jfloat x,
                                                               jfloat y,
                                                               jint button_state,
                                                               jfloat vscroll,
                                                               jfloat hscroll,
                                                               jlong event_time_ms) {
  if (!g_app.input) return JNI_FALSE;
  bool handled = g_app.input->OnAndroidMotionEvent(
      action, x, y, button_state, vscroll, hscroll, static_cast<uint32_t>(event_time_ms));
  return handled ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_google_example_vkwlf_MainActivity_nativeOnKeyEvent(JNIEnv* /*env*/,
                                                            jobject /*thiz*/,
                                                            jint action,
                                                            jint keycode,
                                                            jint meta_state,
                                                            jlong event_time_ms) {
  if (!g_app.input) return JNI_FALSE;
  bool handled = g_app.input->OnAndroidKeyEvent(action, keycode, meta_state,
                                                static_cast<uint32_t>(event_time_ms));
  return handled ? JNI_TRUE : JNI_FALSE;
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

  EnsureServerStarted(sock_path, vsock_port, tcp_port,
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
