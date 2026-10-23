#include <arpa/inet.h>
#include <errno.h>
#include <linux/vm_sockets.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "log_macros.h"
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
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host_ip.c_str(), &addr.sin_addr) <= 0) {
    close(fd);
    return -1;
  }

  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
    LOGI(kTag, "Connected to host VkWlfCompositor over TCP (%s:%u)", host_ip.c_str(), port);
    return fd;
  }
  close(fd);
  return -1;
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
    int fd = ConnectTcp(host_ip, tcp_port);
    if (fd >= 0) return fd;
  }
  if (tcp_port > 0 && host_ip != "127.0.0.1") {
    int fd = ConnectTcp("127.0.0.1", tcp_port);
    if (fd >= 0) return fd;
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

  int host_fd = -1;
  uint64_t forwarded_frames = 0;
  vkwlf::CommittedFrame last_toplevel_frame;
  bool has_last_toplevel = false;

  if (max_frames > 0) {
    host_fd = ConnectToHostCompositor(vsock_cid, vsock_port, host_ip, tcp_port);
    if (host_fd < 0) {
      LOGE(kTag, "Failed to connect to host VkWlfCompositor");
      guest_server.Stop();
      return 1;
    }
  }

  while (g_running.load()) {
    if (host_fd < 0) {
      // Drain any frames produced while disconnected so the queue stays fresh,
      // keeping the latest toplevel frame cached for instant display on connect.
      vkwlf::CommittedFrame pending;
      while (guest_server.PopCommittedFrame(&pending, 0)) {
        if (!pending.is_popup && !pending.is_subsurface && !pending.is_unmapped &&
            !pending.shm_pixels.empty() && pending.parent_surface_id == 0 &&
            pending.width >= (width * 3) / 4) {
          if (last_toplevel_frame.dmabuf_fd >= 0) close(last_toplevel_frame.dmabuf_fd);
          for (auto& p : last_toplevel_frame.planes) {
            if (p.fd >= 0) close(p.fd);
          }
          last_toplevel_frame = std::move(pending);
          has_last_toplevel = true;
        } else {
          if (pending.dmabuf_fd >= 0) close(pending.dmabuf_fd);
          for (auto& p : pending.planes) {
            if (p.fd >= 0) close(p.fd);
          }
        }
      }

      host_fd = ConnectToHostCompositor(vsock_cid, vsock_port, host_ip, tcp_port);
      if (host_fd < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      if (has_last_toplevel) {
        vkwlf::WaylandServerEngine::SendFrameOverBridgeSocket(host_fd, last_toplevel_frame);
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
        bool sent = vkwlf::WaylandServerEngine::SendFrameOverBridgeSocket(host_fd, frame);
        if (!frame.is_popup && !frame.is_subsurface && !frame.is_unmapped &&
            !frame.shm_pixels.empty() && frame.parent_surface_id == 0 &&
            frame.width >= (width * 3) / 4) {
          if (last_toplevel_frame.dmabuf_fd >= 0) close(last_toplevel_frame.dmabuf_fd);
          for (auto& p : last_toplevel_frame.planes) {
            if (p.fd >= 0) close(p.fd);
          }
          last_toplevel_frame = std::move(frame);
          has_last_toplevel = true;
        } else {
          if (frame.dmabuf_fd >= 0) close(frame.dmabuf_fd);
          for (auto& p : frame.planes) {
            if (p.fd >= 0) close(p.fd);
          }
        }
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

  if (last_toplevel_frame.dmabuf_fd >= 0) close(last_toplevel_frame.dmabuf_fd);
  for (auto& p : last_toplevel_frame.planes) {
    if (p.fd >= 0) close(p.fd);
  }
  guest_server.Stop();
  LOGI(kTag, "AVF Guest Wayland Proxy exited cleanly (forwarded_frames=%llu)",
       static_cast<unsigned long long>(forwarded_frames));
  return 0;
}
