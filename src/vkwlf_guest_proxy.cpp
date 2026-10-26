#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "log_macros.h"
#include "vk_renderer.h"
#include "wayland_server_engine.h"

namespace {

constexpr const char* kTag = "VkWlfGuestProxy";
std::atomic<bool> g_running{true};

void OnSignal(int /*sig*/) { g_running.store(false); }

int ConnectVsock(unsigned int cid, unsigned int port) {
  int fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;

  struct sockaddr_vm addr {};
  addr.svm_family = AF_VSOCK;
  addr.svm_cid = cid;
  addr.svm_port = port;

  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
    LOGI(kTag, "Connected to host VkWlfCompositor over AF_VSOCK (cid=%u, port=%u)", cid, port);
    return fd;
  }
  close(fd);
  return -1;
}

int ConnectTcp(const std::string& host_ip, uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host_ip.c_str(), &addr.sin_addr) <= 0) {
    close(fd);
    return -1;
  }

  int res = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  if (res < 0 && errno != EINPROGRESS) {
    close(fd);
    return -1;
  }
  if (res < 0) {
    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (poll(&pfd, 1, 200) <= 0 || !(pfd.revents & POLLOUT)) {
      close(fd);
      return -1;
    }
    int so_err = 0;
    socklen_t err_len = sizeof(so_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &err_len) < 0 || so_err != 0) {
      close(fd);
      return -1;
    }
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  }
  LOGI(kTag, "Connected to host VkWlfCompositor over TCP (%s:%u)", host_ip.c_str(), port);
  return fd;
}

int ConnectToHostCompositor(unsigned int vsock_cid,
                            unsigned int vsock_port,
                            const std::string& host_ip,
                            uint16_t tcp_port) {
  if (vsock_port > 0) {
    int fd = ConnectVsock(vsock_cid, vsock_port);
    if (fd >= 0) return fd;
    if (vsock_cid != VMADDR_CID_LOCAL) {
      fd = ConnectVsock(VMADDR_CID_LOCAL, vsock_port);
      if (fd >= 0) return fd;
    }
  }
  if (tcp_port > 0 && !host_ip.empty()) {
    for (uint16_t try_port : {tcp_port, static_cast<uint16_t>(tcp_port + 1)}) {
      int fd = ConnectTcp(host_ip, try_port);
      if (fd >= 0) return fd;
    }
  }
  if (tcp_port > 0 && host_ip != "127.0.0.1") {
    for (uint16_t try_port : {tcp_port, static_cast<uint16_t>(tcp_port + 1)}) {
      int fd = ConnectTcp("127.0.0.1", try_port);
      if (fd >= 0) return fd;
    }
  }
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  std::string socket_path = "/tmp/wayland-guest-0";
  if (const char* xdg = std::getenv("XDG_RUNTIME_DIR")) {
    if (xdg[0] != '\0') {
      socket_path = std::string(xdg) + "/wayland-0";
    }
  }

  unsigned int vsock_cid = VMADDR_CID_HOST;  // 2 = Host CID in crosvm / AVF
  unsigned int vsock_port = 5555;
  std::string host_ip = "192.168.128.1";  // Default AVF host gateway IP
  uint16_t tcp_port = 5555;
  uint32_t width = 1280;
  uint32_t height = 720;
  uint64_t max_frames = 0;
  std::string dump_ppm_path;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (strcmp(argv[i], "--vsock-cid") == 0 && i + 1 < argc) {
      vsock_cid = static_cast<unsigned int>(std::atoi(argv[++i]));
    } else if (strcmp(argv[i], "--vsock-port") == 0 && i + 1 < argc) {
      vsock_port = static_cast<unsigned int>(std::atoi(argv[++i]));
    } else if (strcmp(argv[i], "--host-ip") == 0 && i + 1 < argc) {
      host_ip = argv[++i];
    } else if (strcmp(argv[i], "--tcp-port") == 0 && i + 1 < argc) {
      tcp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
      width = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
      height = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc) {
      max_frames = static_cast<uint64_t>(std::atoll(argv[++i]));
    } else if (strcmp(argv[i], "--dump-ppm") == 0 && i + 1 < argc) {
      dump_ppm_path = argv[++i];
    }
  }

  LOGI(kTag,
       "Starting AVF Guest Wayland Proxy on '%s' -> Host (vsock=%u:%u, tcp=%s:%u)",
       socket_path.c_str(), vsock_cid, vsock_port, host_ip.c_str(), tcp_port);

  vkwlf::WaylandServerEngine::Config cfg;
  cfg.socket_path = socket_path;
  cfg.vsock_port = -1;
  cfg.tcp_port = -1;
  cfg.display_width = width;
  cfg.display_height = height;

  vkwlf::WaylandServerEngine guest_server(cfg);
  if (!guest_server.Initialize() || !guest_server.Start()) {
    LOGE(kTag, "Failed to initialize guest WaylandServerEngine at '%s'", socket_path.c_str());
    return 1;
  }

  vkwlf::VkRenderer::Config vk_cfg;
  vk_cfg.width = width;
  vk_cfg.height = height;
  vkwlf::VkRenderer guest_renderer(vk_cfg);
  bool guest_vk_ready = guest_renderer.Initialize(nullptr);
  if (guest_vk_ready) {
    LOGI(kTag, "Initialized Guest Vulkan Compositor (%ux%u) for zero-copy zwp_linux_dmabuf_v1 import",
         width, height);
  }

  std::mutex vk_queue_mu;
  std::condition_variable vk_queue_cv;
  vkwlf::CommittedFrame pending_vk_frame;
  bool has_pending_vk_frame = false;

  auto EnqueueGuestVulkanFrame = [&](const vkwlf::CommittedFrame& src) {
    if (!guest_vk_ready) return;
    vkwlf::CommittedFrame copy;
    copy.surface_id = src.surface_id;
    copy.parent_surface_id = src.parent_surface_id;
    copy.frame_number = src.frame_number;
    copy.title = src.title;
    copy.app_id = src.app_id;
    copy.x = src.x;
    copy.y = src.y;
    copy.width = src.width;
    copy.height = src.height;
    copy.stride = src.stride;
    copy.format = src.format;
    copy.drm_modifier = src.drm_modifier;
    copy.buffer_type = src.buffer_type;
    copy.is_popup = src.is_popup;
    copy.is_subsurface = src.is_subsurface;
    copy.is_unmapped = src.is_unmapped;
    copy.dmabuf_fd = (src.dmabuf_fd >= 0) ? dup(src.dmabuf_fd) : -1;
    for (const auto& p : src.planes) {
      copy.planes.push_back(vkwlf::DmaBufPlane{
          .fd = (p.fd >= 0) ? dup(p.fd) : -1,
          .offset = p.offset,
          .stride = p.stride,
          .modifier = p.modifier,
      });
    }
    if (copy.dmabuf_fd < 0 && !src.shm_pixels.empty()) {
      copy.shm_pixels = src.shm_pixels;
    }
    {
      std::lock_guard<std::mutex> lock(vk_queue_mu);
      if (has_pending_vk_frame) {
        if (pending_vk_frame.dmabuf_fd >= 0) close(pending_vk_frame.dmabuf_fd);
        for (auto& p : pending_vk_frame.planes) {
          if (p.fd >= 0) close(p.fd);
        }
      }
      pending_vk_frame = std::move(copy);
      has_pending_vk_frame = true;
    }
    vk_queue_cv.notify_one();
  };

  std::thread guest_vk_worker([&]() {
    while (g_running.load()) {
      vkwlf::CommittedFrame vk_frame;
      {
        std::unique_lock<std::mutex> lock(vk_queue_mu);
        vk_queue_cv.wait_for(lock, std::chrono::milliseconds(50),
                             [&]() { return has_pending_vk_frame || !g_running.load(); });
        if (!has_pending_vk_frame) continue;
        vk_frame = std::move(pending_vk_frame);
        has_pending_vk_frame = false;
      }
      if (guest_vk_ready) {
        guest_renderer.ProcessAndPresentFrame(vk_frame);
        auto stats = guest_renderer.GetStats();
        if (stats.presents_succeeded <= 5 || (stats.presents_succeeded % 60 == 0)) {
          LOGI(kTag,
               "[GuestVulkanGPU] Composited Wayland frame #%llu (surface=%u, buffer_type=%s, "
               "shm_uploads=%llu, dmabuf_imports=%llu, presents=%llu)",
               static_cast<unsigned long long>(vk_frame.frame_number), vk_frame.surface_id,
               vk_frame.buffer_type == vkwlf::BufferType::kDmaBuf ? "DMA-BUF" : "SHM",
               static_cast<unsigned long long>(stats.shm_uploads),
               static_cast<unsigned long long>(stats.dmabuf_imports),
               static_cast<unsigned long long>(stats.presents_succeeded));
        }
      }
    }
  });

  int host_fd = -1;
  uint64_t forwarded_frames = 0;
  std::map<uint32_t, vkwlf::CommittedFrame> last_toplevel_frames;
  std::unordered_map<uint32_t, std::vector<uint8_t>> surface_icons;

  auto CacheToplevelFrame = [&](vkwlf::CommittedFrame&& f) {
    if (!f.icon_png.empty()) {
      surface_icons[f.surface_id] = f.icon_png;
    } else {
      auto icon_it = surface_icons.find(f.surface_id);
      if (icon_it != surface_icons.end()) {
        f.icon_png = icon_it->second;
      }
    }
    if (f.is_unmapped) {
      auto it = last_toplevel_frames.find(f.surface_id);
      if (it != last_toplevel_frames.end()) {
        if (it->second.dmabuf_fd >= 0) close(it->second.dmabuf_fd);
        for (auto& p : it->second.planes) {
          if (p.fd >= 0) close(p.fd);
        }
        last_toplevel_frames.erase(it);
      }
      if (f.dmabuf_fd >= 0) close(f.dmabuf_fd);
      for (auto& p : f.planes) {
        if (p.fd >= 0) close(p.fd);
      }
      return;
    }
    if (!f.is_popup &&
        (!f.shm_pixels.empty() || f.dmabuf_fd >= 0) &&
        (f.parent_surface_id == 0 || f.is_subsurface) &&
        f.width >= 320 && f.height >= 240) {
      auto it = last_toplevel_frames.find(f.surface_id);
      if (it != last_toplevel_frames.end()) {
        if (it->second.dmabuf_fd >= 0) close(it->second.dmabuf_fd);
        for (auto& p : it->second.planes) {
          if (p.fd >= 0) close(p.fd);
        }
      }
      last_toplevel_frames[f.surface_id] = std::move(f);
    } else {
      if (f.dmabuf_fd >= 0) close(f.dmabuf_fd);
      for (auto& p : f.planes) {
        if (p.fd >= 0) close(p.fd);
      }
    }
  };

  if (max_frames > 0) {
    host_fd = ConnectToHostCompositor(vsock_cid, vsock_port, host_ip, tcp_port);
    if (host_fd < 0) {
      LOGE(kTag, "Failed to connect to host VkWlfCompositor");
      g_running.store(false);
      vk_queue_cv.notify_all();
      if (guest_vk_worker.joinable()) guest_vk_worker.join();
      if (guest_vk_ready) guest_renderer.Shutdown();
      guest_server.Stop();
      return 1;
    }
  }

  while (g_running.load()) {
    if (host_fd < 0) {
      // Drain any frames produced while disconnected so the queue stays fresh,
      // keeping the latest toplevel frame per app cached for instant display on connect.
      vkwlf::CommittedFrame pending;
      while (guest_server.PopCommittedFrame(&pending, 0)) {
        EnqueueGuestVulkanFrame(pending);
        CacheToplevelFrame(std::move(pending));
      }

      host_fd = ConnectToHostCompositor(vsock_cid, vsock_port, host_ip, tcp_port);
      if (host_fd < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      auto catalog = vkwlf::WaylandServerEngine::DiscoverLocalGuestAppCatalog();
      vkwlf::WaylandServerEngine::SendAppCatalogOverBridgeSocket(host_fd, catalog);

      for (const auto& [sid, cached_frame] : last_toplevel_frames) {
        (void)sid;
        if (!cached_frame.is_subsurface) {
          vkwlf::WaylandServerEngine::SendFrameOverBridgeSocket(host_fd, cached_frame);
        }
      }
      for (const auto& [sid, cached_frame] : last_toplevel_frames) {
        (void)sid;
        if (cached_frame.is_subsurface) {
          vkwlf::WaylandServerEngine::SendFrameOverBridgeSocket(host_fd, cached_frame);
        }
      }
    }

    std::atomic<bool> conn_alive{true};
    std::thread input_reader([&guest_server, host_fd, &conn_alive]() {
      while (g_running.load() && conn_alive.load()) {
        if (!guest_server.ProcessBridgeInputFromHost(host_fd)) {
          conn_alive.store(false);
          break;
        }
      }
    });

    while (g_running.load() && conn_alive.load()) {
      vkwlf::CommittedFrame frame;
      if (guest_server.PopCommittedFrame(&frame, 16)) {
        if (frame.icon_png.empty()) {
          auto icon_it = surface_icons.find(frame.surface_id);
          if (icon_it != surface_icons.end()) {
            frame.icon_png = icon_it->second;
          }
        }
        bool sent = vkwlf::WaylandServerEngine::SendFrameOverBridgeSocket(host_fd, frame);
        EnqueueGuestVulkanFrame(frame);
        CacheToplevelFrame(std::move(frame));
        if (!sent) {
          LOGI(kTag, "Host VkWlfCompositor disconnected; waiting for reconnect...");
          conn_alive.store(false);
          break;
        }
        forwarded_frames++;
        if (max_frames > 0 && forwarded_frames >= max_frames) {
          LOGI(kTag, "Forwarded target max_frames=%llu to host compositor",
               static_cast<unsigned long long>(forwarded_frames));
          g_running.store(false);
          break;
        }
      }
    }

    conn_alive.store(false);
    shutdown(host_fd, SHUT_RDWR);
    close(host_fd);
    host_fd = -1;
    if (input_reader.joinable()) {
      input_reader.join();
    }
    if (max_frames > 0) {
      break;
    }
  }

  g_running.store(false);
  vk_queue_cv.notify_all();
  if (guest_vk_worker.joinable()) {
    guest_vk_worker.join();
  }
  {
    std::lock_guard<std::mutex> lock(vk_queue_mu);
    if (has_pending_vk_frame) {
      if (pending_vk_frame.dmabuf_fd >= 0) close(pending_vk_frame.dmabuf_fd);
      for (auto& p : pending_vk_frame.planes) {
        if (p.fd >= 0) close(p.fd);
      }
      has_pending_vk_frame = false;
    }
  }
  if (guest_vk_ready) {
    if (!dump_ppm_path.empty()) {
      guest_renderer.SaveLastFramePpm(dump_ppm_path);
    }
    guest_renderer.Shutdown();
  }
  for (auto& [sid, cached_frame] : last_toplevel_frames) {
    (void)sid;
    if (cached_frame.dmabuf_fd >= 0) close(cached_frame.dmabuf_fd);
    for (auto& p : cached_frame.planes) {
      if (p.fd >= 0) close(p.fd);
    }
  }
  guest_server.Stop();
  LOGI(kTag, "AVF Guest Wayland Proxy exited cleanly (forwarded_frames=%llu)",
       static_cast<unsigned long long>(forwarded_frames));
  return 0;
}
