#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace vkwlf {

enum class BufferType {
  kShm = 0,
  kDmaBuf = 1,
};

struct DmaBufPlane {
  int fd = -1;
  uint32_t offset = 0;
  uint32_t stride = 0;
  uint64_t modifier = 0;
};

struct CommittedFrame {
  uint32_t surface_id = 0;
  uint32_t parent_surface_id = 0;
  uint64_t frame_number = 0;
  std::string title;
  std::string app_id;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t format = 0;  // wl_shm_format or DRM fourcc
  BufferType buffer_type = BufferType::kShm;
  bool is_popup = false;
  bool is_subsurface = false;
  bool is_unmapped = false;

  // Populated when buffer_type == BufferType::kShm
  std::vector<uint8_t> shm_pixels;

  // Optional PNG icon bytes resolved from the Linux application's .desktop / icon theme
  std::vector<uint8_t> icon_png;

  // Populated when buffer_type == BufferType::kDmaBuf
  int dmabuf_fd = -1;
  uint64_t drm_modifier = 0;
  std::vector<DmaBufPlane> planes;
};

struct GuestAppCatalogEntry {
  std::string desktop_id;
  std::string name;
  std::string comment;
  std::string exec;
  std::string apt_package;
  bool installed = true;
  std::vector<uint8_t> icon_png;
};

class WaylandServerEngine {
 public:
  struct Config {
    std::string socket_path = "/data/local/tmp/wayland-0";
    int vsock_port = -1;  // Optional AF_VSOCK port for crosvm guest connections
    int tcp_port = -1;    // Optional TCP port for guest bridge connections
    uint32_t display_width = 1280;
    uint32_t display_height = 720;
    uint32_t refresh_mhz = 60000;
  };

  using AppLaunchedCallback = std::function<void(uint32_t surface_id,
                                                 const std::string& title,
                                                 const std::string& app_id,
                                                 const std::vector<uint8_t>& icon_png)>;
  using AppCatalogCallback = std::function<void(const std::vector<GuestAppCatalogEntry>& entries)>;

  explicit WaylandServerEngine(const Config& config);
  ~WaylandServerEngine();

  WaylandServerEngine(const WaylandServerEngine&) = delete;
  WaylandServerEngine& operator=(const WaylandServerEngine&) = delete;

  // Initializes wl_display, protocols, and binds the socket.
  bool Initialize();

  // Starts the dedicated background Wayland event-loop thread.
  bool Start();

  // Stops the background thread and tears down resources.
  void Stop();

  // Registers a callback invoked when a guest Wayland application (e.g. VS Code-class IDEs)
  // creates and commits a toplevel window, allowing the Android host to auto-foreground the UI.
  void SetAppLaunchedCallback(AppLaunchedCallback cb);

  // Registers a callback invoked when the guest VM sends or updates its catalog of available Linux
  // GUI applications (.desktop entries + installable candidates).
  void SetAppCatalogCallback(AppCatalogCallback cb);

  // Returns the latest cached catalog of guest Linux applications.
  std::vector<GuestAppCatalogEntry> GetGuestAppCatalog() const;

  // Requests the connected guest VM proxy to launch (or install & launch) a Linux application, or
  // refresh the catalog ("__REFRESH_CATALOG__").
  void RequestGuestAppLaunch(const std::string& launch_spec);

  // Discovers installed .desktop entries + curated installable Linux GUI apps on the local (guest)
  // system and serializes/sends the catalog over a bridge socket to the Android host.
  static std::vector<GuestAppCatalogEntry> DiscoverLocalGuestAppCatalog();
  static bool SendAppCatalogOverBridgeSocket(int sock_fd,
                                             const std::vector<GuestAppCatalogEntry>& entries);

  // Thread-safe frame queue operations for the Vulkan renderer thread.
  void PushCommittedFrame(CommittedFrame&& frame);
  bool PopCommittedFrame(CommittedFrame* out_frame, uint32_t timeout_ms = 0);
  size_t PendingFrameCount() const;

  // Input injection APIs (safe to call from any thread; marshaled to Wayland loop and any
  // connected remote guest proxies).
  void SendPointerMotion(uint32_t time_ms, double x, double y);
  void SendPointerButton(uint32_t time_ms, uint32_t button, uint32_t state);
  void SendPointerAxis(uint32_t time_ms, uint32_t axis, double value);
  void SendKeyboardKey(uint32_t time_ms, uint32_t key, uint32_t state);
  void SendKeyboardModifiers(uint32_t mods_depressed,
                             uint32_t mods_latched,
                             uint32_t mods_locked,
                             uint32_t group);

  // Updates advertised output/toplevel dimensions on surface resize and sends
  // xdg_toplevel.configure(width, height) to active clients.
  void SetDisplaySize(uint32_t width, uint32_t height);
  void SetDisplaySizeForSurface(uint32_t surface_id, uint32_t width, uint32_t height);

  // Binds subsequent input commands to a specific toplevel surface_id and its host window size.
  void SetActiveInputSurface(uint32_t surface_id,
                             uint32_t host_width = 0,
                             uint32_t host_height = 0);
  void FocusSurfaceById(uint32_t surface_id);

  // Sends a serialized CommittedFrame over a stream socket (used by vkwlf_guest_proxy).
  static bool SendFrameOverBridgeSocket(int sock_fd, const CommittedFrame& frame);

  // Reads a remote InputCommand packet from a bridge socket and injects it into this engine.
  bool ProcessBridgeInputFromHost(int sock_fd);

  const std::string& BoundSocketPath() const { return bound_socket_path_; }
  uint32_t NextSerial();

  // Internal accessors for static Wayland C callbacks:
  struct wl_display* display() const { return display_; }
  const Config& config() const { return config_; }
  uint64_t NextFrameNumber() { return ++frame_counter_; }

  void RegisterClientPointer(struct wl_resource* pointer_res);
  void UnregisterClientPointer(struct wl_resource* pointer_res);
  void RegisterClientKeyboard(struct wl_resource* keyboard_res);
  void UnregisterClientKeyboard(struct wl_resource* keyboard_res);
  void RegisterToplevelSurface(struct wl_resource* surface_res,
                               struct wl_resource* xdg_surface_res,
                               struct wl_resource* xdg_toplevel_res);
  void UnregisterToplevelSurface(struct wl_resource* xdg_toplevel_res);
  void SetFocusedSurface(struct wl_resource* surface_res);
  struct wl_resource* FocusedSurface() const { return focused_surface_; }
  struct wl_resource* ResolvePointerTarget(double screen_x,
                                           double screen_y,
                                           double* out_local_x,
                                           double* out_local_y) const;
  void SetToplevelGeometry(int32_t crop_x, int32_t crop_y, uint32_t crop_w, uint32_t crop_h);
  void RecordSurfaceGeometry(uint32_t surface_id,
                             int32_t crop_x,
                             int32_t crop_y,
                             uint32_t crop_w,
                             uint32_t crop_h);
  bool GetSurfaceGeometry(uint32_t surface_id,
                          int32_t* out_crop_x,
                          int32_t* out_crop_y,
                          uint32_t* out_crop_w,
                          uint32_t* out_crop_h) const;
  void OnSurfaceUnmappedOrDestroyed(struct wl_resource* surface_res,
                                    uint32_t surface_id,
                                    bool was_drawable);
  void NotifyAppLaunched(uint32_t surface_id,
                         const std::string& title,
                         const std::string& app_id,
                         const std::vector<uint8_t>& icon_png);
  void NotifyAppCatalogUpdated(std::vector<GuestAppCatalogEntry>&& entries);

 private:
  struct InputCommand {
    enum class Type : uint32_t {
      kPointerMotion = 1,
      kPointerButton = 2,
      kPointerAxis = 3,
      kKeyboardKey = 4,
      kKeyboardModifiers = 5,
      kDisplayResize = 6,
      kQuit = 7,
      kFocusSurface = 8,
      kLaunchApp = 9,
    };
    Type type;
    uint32_t time_ms = 0;
    double x = 0.0;
    double y = 0.0;
    uint32_t button_or_key = 0;
    uint32_t state = 0;
    uint32_t mods_depressed = 0;
    uint32_t mods_latched = 0;
    uint32_t mods_locked = 0;
    uint32_t group = 0;
    uint32_t target_surface_id = 0;
    uint32_t host_width = 0;
    uint32_t host_height = 0;
    std::string launch_spec;
  };

  struct ToplevelEntry {
    struct wl_resource* surface_res = nullptr;
    struct wl_resource* xdg_surface_res = nullptr;
    struct wl_resource* xdg_toplevel_res = nullptr;
  };

  void EventLoopThreadMain();
  void EnqueueInputCommand(const InputCommand& cmd);
  void ProcessPendingInputCommands();
  void ForwardInputCommandToBridgeClients(const InputCommand& cmd);
  static int OnWakeupFdEvent(int fd, uint32_t mask, void* data);
  static int OnVsockAcceptEvent(int fd, uint32_t mask, void* data);
  static int OnTcpAcceptEvent(int fd, uint32_t mask, void* data);
  static int OnBridgeClientDataEvent(int fd, uint32_t mask, void* data);
  bool SetupVsockListener(int port);
  bool SetupTcpListener(int port);
  void AcceptBridgeClient(int listen_fd, const char* transport_name);
  int CreateKeymapFd(size_t* out_size);

  Config config_;
  std::string bound_socket_path_;

  struct wl_display* display_ = nullptr;
  struct wl_event_loop* event_loop_ = nullptr;
  struct wl_event_source* wakeup_source_ = nullptr;
  struct wl_event_source* vsock_source_ = nullptr;
  struct wl_event_source* tcp_source_ = nullptr;
  int wakeup_pipe_[2] = {-1, -1};
  int vsock_fd_ = -1;
  int tcp_fd_ = -1;

  struct BridgeClient {
    int fd = -1;
    struct wl_event_source* source = nullptr;
    WaylandServerEngine* engine = nullptr;
  };
  std::mutex bridge_clients_mutex_;
  std::vector<std::unique_ptr<BridgeClient>> bridge_clients_;

  std::mutex app_cb_mutex_;
  AppLaunchedCallback app_launched_cb_;
  AppCatalogCallback app_catalog_cb_;

  mutable std::mutex app_catalog_mutex_;
  std::vector<GuestAppCatalogEntry> guest_app_catalog_;

  std::thread server_thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frame_counter_{0};

  mutable std::mutex frame_queue_mutex_;
  std::condition_variable frame_queue_cv_;
  std::deque<CommittedFrame> frame_queue_;

  std::mutex input_mutex_;
  std::deque<InputCommand> input_queue_;

  std::vector<struct wl_resource*> pointer_resources_;
  std::vector<struct wl_resource*> keyboard_resources_;
  std::vector<ToplevelEntry> toplevel_entries_;
  struct wl_resource* focused_surface_ = nullptr;
  struct wl_resource* preferred_base_surface_ = nullptr;
  bool pointer_entered_ = false;
  bool keyboard_entered_ = false;
  std::atomic<uint32_t> active_input_surface_id_{0};
  std::atomic<uint32_t> active_input_width_{0};
  std::atomic<uint32_t> active_input_height_{0};
  std::atomic<uint32_t> host_display_width_{0};
  std::atomic<uint32_t> host_display_height_{0};
  std::atomic<int32_t> last_crop_x_{0};
  std::atomic<int32_t> last_crop_y_{0};
  std::atomic<uint32_t> last_crop_w_{0};
  std::atomic<uint32_t> last_crop_h_{0};
  struct SurfaceGeomInfo {
    int32_t crop_x = 0;
    int32_t crop_y = 0;
    uint32_t crop_w = 0;
    uint32_t crop_h = 0;
  };
  mutable std::mutex surface_geom_mutex_;
  std::unordered_map<uint32_t, SurfaceGeomInfo> surface_geometries_;
  double last_pointer_x_ = 0.0;
  double last_pointer_y_ = 0.0;
  uint32_t pressed_buttons_mask_ = 0;
  bool pending_left_press_ = false;
  bool scroll_drag_active_ = false;
  uint32_t down_time_ms_ = 0;
  uint32_t last_button_event_time_ms_ = 0;
  uint32_t last_motion_time_ms_ = 0;
  double down_x_ = 0.0;
  double down_y_ = 0.0;
  double prev_scroll_x_ = 0.0;
  double prev_scroll_y_ = 0.0;
};

}  // namespace vkwlf
