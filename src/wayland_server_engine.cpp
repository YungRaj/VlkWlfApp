#include "wayland_server_engine.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "log_macros.h"
#include "xdg-shell-server-protocol.h"

namespace vkwlf {

namespace {

constexpr const char* kTag = "VkWlfCompositor";

// Standard DRM FourCC codes for zero-copy dmabuf negotiation
constexpr uint32_t kDrmFormatARGB8888 = 0x34325241;  // 'AR24'
constexpr uint32_t kDrmFormatXRGB8888 = 0x34325258;  // 'XR24'
constexpr uint32_t kDrmFormatABGR8888 = 0x34324241;  // 'AB24'
constexpr uint32_t kDrmFormatXBGR8888 = 0x34324258;  // 'XB24'

// Wire magic for AVF VSOCK/TCP Wayland Bridge packets
constexpr uint32_t kBridgeFrameMagic = 0x564B5746;  // 'VKWF'
constexpr uint32_t kBridgeInputMagic = 0x564B5749;  // 'VKWI'

#pragma pack(push, 1)
struct BridgeFrameHeader {
  uint32_t magic = kBridgeFrameMagic;
  uint32_t surface_id = 0;
  uint32_t parent_surface_id = 0;
  uint64_t frame_number = 0;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t format = 0;
  uint32_t buffer_type = 0;
  uint8_t is_popup = 0;
  uint8_t is_subsurface = 0;
  uint8_t is_unmapped = 0;
  uint16_t title_len = 0;
  uint16_t app_id_len = 0;
  uint32_t icon_png_len = 0;
  uint32_t payload_bytes = 0;
};

struct BridgeInputPacket {
  uint32_t magic = kBridgeInputMagic;
  uint32_t type = 0;
  uint32_t time_ms = 0;
  double x = 0.0;
  double y = 0.0;
  uint32_t button_or_key = 0;
  uint32_t state = 0;
  uint32_t mods_depressed = 0;
  uint32_t mods_latched = 0;
  uint32_t mods_locked = 0;
  uint32_t group = 0;
};
#pragma pack(pop)

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>* out) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 8 ||
      st.st_size > 4 * 1024 * 1024) {
    close(fd);
    return false;
  }
  out->resize(static_cast<size_t>(st.st_size));
  ssize_t n = read(fd, out->data(), out->size());
  close(fd);
  if (n != st.st_size) {
    out->clear();
    return false;
  }
  // Verify PNG magic signature
  static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (std::memcmp(out->data(), kPngSig, 8) != 0) {
    out->clear();
    return false;
  }
  return true;
}

std::string ToLowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string TrimAscii(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

std::vector<std::string> GetXdgDataDirs() {
  std::vector<std::string> dirs;
  if (const char* xdg_data_home = std::getenv("XDG_DATA_HOME");
      xdg_data_home && xdg_data_home[0] != '\0') {
    dirs.emplace_back(xdg_data_home);
  } else if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
    dirs.push_back(std::string(home) + "/.local/share");
  }
  if (const char* xdg_data_dirs = std::getenv("XDG_DATA_DIRS");
      xdg_data_dirs && xdg_data_dirs[0] != '\0') {
    std::stringstream ss(xdg_data_dirs);
    std::string item;
    while (std::getline(ss, item, ':')) {
      if (!item.empty()) dirs.push_back(item);
    }
  } else {
    dirs.emplace_back("/usr/local/share");
    dirs.emplace_back("/usr/share");
    dirs.emplace_back("/var/lib/flatpak/exports/share");
    dirs.emplace_back("/var/lib/snapd/desktop");
  }
  return dirs;
}

// Resolves an `Icon=` value (either an absolute path or an icon theme name) to PNG bytes
// using the freedesktop.org Icon Theme Specification.
bool ResolveIconValueToPng(const std::string& icon_val, std::vector<uint8_t>* out_png) {
  if (icon_val.empty()) return false;
  if (icon_val[0] == '/') {
    if (ReadFileBytes(icon_val, out_png)) return true;
    if (icon_val.find('.') == std::string::npos && ReadFileBytes(icon_val + ".png", out_png)) {
      return true;
    }
    return false;
  }

  std::string base = icon_val;
  if (base.size() > 4 && base.substr(base.size() - 4) == ".png") {
    base = base.substr(0, base.size() - 4);
  }
  const std::string lower_base = ToLowerAscii(base);
  std::vector<std::string> base_candidates = {base};
  if (lower_base != base) {
    base_candidates.push_back(lower_base);
  }

  const auto data_dirs = GetXdgDataDirs();
  static const char* kThemes[] = {"hicolor", "Adwaita", "gnome", "breeze", "Papirus"};
  static const char* kSizes[] = {"256x256", "512x512", "128x128", "64x64", "48x48", "32x32"};

  for (const auto& dir : data_dirs) {
    for (const auto& cand : base_candidates) {
      for (const char* theme : kThemes) {
        for (const char* sz : kSizes) {
          std::string p = dir + "/icons/" + theme + "/" + sz + "/apps/" + cand + ".png";
          if (ReadFileBytes(p, out_png)) return true;
        }
      }
      if (ReadFileBytes(dir + "/pixmaps/" + cand + ".png", out_png)) return true;
      if (ReadFileBytes(dir + "/icons/" + cand + ".png", out_png)) return true;
    }
  }
  return false;
}

struct DesktopEntryInfo {
  std::string desktop_id;  // filename without .desktop
  std::string name;
  std::string comment;
  std::string startup_wm_class;
  std::string exec;
  std::string icon;
  bool no_display = false;
  bool terminal = false;
};

bool ParseDesktopFile(const std::string& path,
                      const std::string& filename,
                      DesktopEntryInfo* out) {
  std::ifstream in(path);
  if (!in.is_open()) return false;

  out->desktop_id = filename;
  if (out->desktop_id.size() > 8 &&
      out->desktop_id.substr(out->desktop_id.size() - 8) == ".desktop") {
    out->desktop_id.resize(out->desktop_id.size() - 8);
  }

  bool in_desktop_entry = false;
  std::string line;
  while (std::getline(in, line)) {
    std::string trimmed = TrimAscii(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    if (trimmed.front() == '[' && trimmed.back() == ']') {
      in_desktop_entry = (trimmed == "[Desktop Entry]");
      continue;
    }
    if (!in_desktop_entry) continue;
    size_t eq = trimmed.find('=');
    if (eq == std::string::npos) continue;
    std::string k = TrimAscii(trimmed.substr(0, eq));
    std::string v = TrimAscii(trimmed.substr(eq + 1));
    if (k == "Name" && out->name.empty()) {
      out->name = v;
    } else if (k == "Comment" && out->comment.empty()) {
      out->comment = v;
    } else if (k == "GenericName" && out->comment.empty()) {
      out->comment = v;
    } else if (k == "StartupWMClass" && out->startup_wm_class.empty()) {
      out->startup_wm_class = v;
    } else if (k == "Exec" && out->exec.empty()) {
      out->exec = v;
    } else if (k == "Icon" && out->icon.empty()) {
      out->icon = v;
    } else if (k == "NoDisplay" || k == "Hidden") {
      if (ToLowerAscii(v) == "true") out->no_display = true;
    } else if (k == "Terminal") {
      if (ToLowerAscii(v) == "true") out->terminal = true;
    }
  }
  return !out->name.empty() && (!out->icon.empty() || !out->exec.empty());
}

// Inspects /proc/<pid>/exe for a connected Wayland client and searches for PNG icons inside
// the application's own bundle directory (e.g., Electron/Chromium/custom apps not in /usr/share).
bool ResolveIconFromClientPid(pid_t pid,
                              std::string* out_exe_path,
                              std::string* out_exe_basename,
                              std::vector<uint8_t>* out_png) {
  if (pid <= 0) return false;
  char link_buf[4096] = {};
  std::string proc_exe = "/proc/" + std::to_string(pid) + "/exe";
  ssize_t len = readlink(proc_exe.c_str(), link_buf, sizeof(link_buf) - 1);
  if (len <= 0) return false;
  std::string exe_path(link_buf, static_cast<size_t>(len));
  if (out_exe_path) *out_exe_path = exe_path;

  size_t slash = exe_path.find_last_of('/');
  std::string exe_dir = (slash != std::string::npos) ? exe_path.substr(0, slash) : "";
  std::string exe_base = (slash != std::string::npos) ? exe_path.substr(slash + 1) : exe_path;
  if (out_exe_basename) *out_exe_basename = exe_base;

  if (exe_dir.empty()) return false;

  // Common relative icon locations inside self-contained Linux app bundles (Electron, Qt, AppImage)
  const std::vector<std::string> bundle_rel_Dirs = {
      exe_dir + "/resources/app/resources/linux",
      exe_dir + "/resources/app/resources",
      exe_dir + "/resources",
      exe_dir + "/icons",
      exe_dir + "/../share/icons/hicolor/256x256/apps",
      exe_dir + "/../share/pixmaps",
      exe_dir,
  };

  const std::string lower_exe_base = ToLowerAscii(exe_base);
  for (const auto& dir_path : bundle_rel_Dirs) {
    const bool is_shared_dir = (dir_path.find("../share/") != std::string::npos);
    DIR* d = opendir(dir_path.c_str());
    if (!d) continue;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
      std::string fname = ent->d_name;
      std::string lower_fname = ToLowerAscii(fname);
      if (fname.size() > 4 && lower_fname.substr(lower_fname.size() - 4) == ".png") {
        if (is_shared_dir && !lower_exe_base.empty() &&
            lower_fname.find(lower_exe_base) == std::string::npos) {
          continue;
        }
        std::string full = dir_path + "/" + fname;
        if (ReadFileBytes(full, out_png)) {
          closedir(d);
          LOGI(kTag, "Resolved bundle icon '%s' (%zu bytes) via client PID %d (%s)", full.c_str(),
               out_png->size(), static_cast<int>(pid), exe_path.c_str());
          return true;
        }
      }
    }
    closedir(d);
  }
  return false;
}

// When an X11 application is running inside a rootful Xwayland server (e.g. "Xwayland on :15"),
// inspects /proc/*/environ for a GUI client connected to that DISPLAY and resolves its real
// application Name and .desktop ID (e.g. "TeamViewer" / "com.teamviewer.TeamViewer").
bool ResolveXwaylandClientMetadata(const std::string& xwayland_display,
                                   std::string* out_title,
                                   std::string* out_app_id) {
  if (xwayland_display.empty()) return false;
  const std::string target_env = "DISPLAY=" + xwayland_display;

  DIR* proc_d = opendir("/proc");
  if (!proc_d) return false;

  std::string matched_exe_path;
  std::string matched_exe_base;
  struct dirent* ent = nullptr;
  while ((ent = readdir(proc_d)) != nullptr) {
    const char* name = ent->d_name;
    if (name[0] < '1' || name[0] > '9') continue;
    pid_t pid = static_cast<pid_t>(std::atoi(name));
    if (pid <= 0) continue;

    std::string env_path = std::string("/proc/") + name + "/environ";
    int fd = open(env_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) continue;

    bool has_display = false;
    for (ssize_t i = 0; i < n;) {
      std::string var(buf + i);
      i += static_cast<ssize_t>(var.size()) + 1;
      if (var == target_env) {
        has_display = true;
        break;
      }
    }
    if (!has_display) continue;

    char link_buf[4096] = {};
    std::string proc_exe = std::string("/proc/") + name + "/exe";
    ssize_t len = readlink(proc_exe.c_str(), link_buf, sizeof(link_buf) - 1);
    if (len <= 0) continue;
    std::string exe_path(link_buf, static_cast<size_t>(len));
    size_t slash = exe_path.find_last_of('/');
    std::string exe_base = (slash != std::string::npos) ? exe_path.substr(slash + 1) : exe_path;
    std::string low_base = ToLowerAscii(exe_base);

    if (low_base == "xwayland" || low_base == "xkbcomp" || low_base == "sh" ||
        low_base == "bash" || low_base == "dash" || low_base.rfind("python", 0) == 0 ||
        low_base == "weston" || low_base == "openbox" || low_base == "env" ||
        low_base == "timeout") {
      continue;
    }

    matched_exe_path = exe_path;
    matched_exe_base = exe_base;
    break;
  }
  closedir(proc_d);

  if (matched_exe_base.empty()) return false;

  const std::string low_exe_base = ToLowerAscii(matched_exe_base);
  const auto data_dirs = GetXdgDataDirs();
  for (const auto& base_dir : data_dirs) {
    std::string app_dir = base_dir + "/applications";
    DIR* d = opendir(app_dir.c_str());
    if (!d) continue;
    struct dirent* dent = nullptr;
    while ((dent = readdir(d)) != nullptr) {
      std::string fname = dent->d_name;
      if (fname.size() <= 8 || fname.substr(fname.size() - 8) != ".desktop") continue;
      DesktopEntryInfo info;
      if (!ParseDesktopFile(app_dir + "/" + fname, fname, &info)) continue;

      std::string id_low = ToLowerAscii(info.desktop_id);
      std::string name_low = ToLowerAscii(info.name);
      std::string exec_low = ToLowerAscii(info.exec);
      if (id_low.find(low_exe_base) != std::string::npos ||
          name_low == low_exe_base ||
          exec_low.find(low_exe_base) != std::string::npos) {
        if (out_title && !info.name.empty()) *out_title = info.name;
        if (out_app_id && !info.desktop_id.empty()) *out_app_id = info.desktop_id;
        closedir(d);
        return true;
      }
    }
    closedir(d);
  }

  if (out_title) *out_title = matched_exe_base;
  if (out_app_id) *out_app_id = low_exe_base;
  return true;
}

// Agnostically resolves any Linux Wayland application's PNG icon using:
// 1. Standard XDG .desktop entries ($XDG_DATA_HOME/applications, $XDG_DATA_DIRS/applications)
//    matched against xdg_toplevel.app_id, StartupWMClass, Name, Title, or /proc/<pid>/exe.
// 2. Freedesktop.org Icon Theme Specification lookup ($XDG_DATA_DIRS/icons, pixmaps).
// 3. Direct /proc/<pid>/exe bundle directory inspection via wl_client_get_credentials.
std::vector<uint8_t> ResolveLinuxAppIconPng(struct wl_client* client,
                                            const std::string& app_id,
                                            const std::string& title) {
  pid_t client_pid = 0;
  if (client) {
    uid_t uid = 0;
    gid_t gid = 0;
    wl_client_get_credentials(client, &client_pid, &uid, &gid);
  }

  std::string exe_path;
  std::string exe_basename;
  if (client_pid > 0) {
    char link_buf[4096] = {};
    std::string proc_exe = "/proc/" + std::to_string(client_pid) + "/exe";
    ssize_t len = readlink(proc_exe.c_str(), link_buf, sizeof(link_buf) - 1);
    if (len > 0) {
      exe_path.assign(link_buf, static_cast<size_t>(len));
      size_t slash = exe_path.find_last_of('/');
      exe_basename = (slash != std::string::npos) ? exe_path.substr(slash + 1) : exe_path;
    }
  }

  std::string cache_key =
      ToLowerAscii(app_id + "|" + title + "|" + exe_path);
  static std::mutex cache_mu;
  static std::unordered_map<std::string, std::vector<uint8_t>> icon_cache;
  {
    std::lock_guard<std::mutex> lock(cache_mu);
    auto it = icon_cache.find(cache_key);
    if (it != icon_cache.end() && !it->second.empty()) {
      return it->second;
    }
  }

  std::string lower_app = ToLowerAscii(app_id);
  std::string lower_app_short = lower_app;
  if (size_t dot = lower_app_short.find_last_of('.'); dot != std::string::npos && dot + 1 < lower_app_short.size()) {
    lower_app_short = lower_app_short.substr(dot + 1);
  }
  std::string lower_title = ToLowerAscii(title);
  std::string lower_exe_base = ToLowerAscii(exe_basename);

  std::vector<uint8_t> png_bytes;

  // Step 1: Scan all XDG .desktop files and match against app_id, StartupWMClass, Exec, or Name
  const auto data_dirs = GetXdgDataDirs();
  std::string best_icon_value;
  int best_score = 0;

  for (const auto& base_dir : data_dirs) {
    std::string app_dir = base_dir + "/applications";
    DIR* d = opendir(app_dir.c_str());
    if (!d) continue;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
      std::string fname = ent->d_name;
      if (fname.size() <= 8 || fname.substr(fname.size() - 8) != ".desktop") continue;
      DesktopEntryInfo info;
      if (!ParseDesktopFile(app_dir + "/" + fname, fname, &info)) continue;

      std::string id_low = ToLowerAscii(info.desktop_id);
      std::string wm_low = ToLowerAscii(info.startup_wm_class);
      std::string name_low = ToLowerAscii(info.name);
      std::string exec_low = ToLowerAscii(info.exec);

      int score = 0;
      if (!lower_app.empty()) {
        if (id_low == lower_app || wm_low == lower_app ||
            (!lower_app_short.empty() && (id_low == lower_app_short || wm_low == lower_app_short))) {
          score = 100;
        } else if (id_low.find(lower_app) != std::string::npos ||
                   lower_app.find(id_low) != std::string::npos ||
                   (!lower_app_short.empty() && lower_app_short.size() >= 3 &&
                    (id_low.find(lower_app_short) != std::string::npos ||
                     (!wm_low.empty() && wm_low.find(lower_app_short) != std::string::npos))) ||
                   (!wm_low.empty() && (wm_low.find(lower_app) != std::string::npos ||
                                        lower_app.find(wm_low) != std::string::npos))) {
          score = std::max(score, 80);
        }
      }
      if (! exe_path.empty() && exec_low.find(ToLowerAscii(exe_path)) != std::string::npos) {
        score = std::max(score, 95);
      } else if (!lower_exe_base.empty() && lower_exe_base.size() >= 3 &&
                 (id_low == lower_exe_base || wm_low == lower_exe_base ||
                  exec_low.find("/" + lower_exe_base) != std::string::npos)) {
        score = std::max(score, 85);
      }
      if (!lower_title.empty() && !name_low.empty() &&
          (lower_title == name_low || lower_title.find(name_low) != std::string::npos)) {
        score = std::max(score, 70);
      }

      if (score > best_score) {
        std::vector<uint8_t> candidate_png;
        if (ResolveIconValueToPng(info.icon, &candidate_png)) {
          best_score = score;
          best_icon_value = info.icon;
          png_bytes = std::move(candidate_png);
        }
      }
    }
    closedir(d);
  }

  // Step 2: Direct freedesktop Icon Theme lookup using app_id or executable basename
  if (png_bytes.empty() && !lower_app.empty()) {
    ResolveIconValueToPng(lower_app, &png_bytes);
  }
  if (png_bytes.empty() && !lower_app_short.empty() && lower_app_short != lower_app) {
    ResolveIconValueToPng(lower_app_short, &png_bytes);
  }
  if (png_bytes.empty() && !lower_exe_base.empty()) {
    ResolveIconValueToPng(lower_exe_base, &png_bytes);
  }

  // Step 3: Inspect the running process's own bundle directory via /proc/<pid>/exe
  if (png_bytes.empty() && client_pid > 0) {
    ResolveIconFromClientPid(client_pid, nullptr, nullptr, &png_bytes);
  }

  if (!png_bytes.empty()) {
    LOGI(kTag,
         "Agnostically resolved Linux app icon (%zu bytes, match_score=%d, icon='%s') for "
         "app_id='%s' title='%s' pid=%d exe='%s'",
         png_bytes.size(), best_score, best_icon_value.c_str(), app_id.c_str(), title.c_str(),
         static_cast<int>(client_pid), exe_path.c_str());
    std::lock_guard<std::mutex> lock(cache_mu);
    icon_cache[cache_key] = png_bytes;
  }

  return png_bytes;
}

bool WriteAll(int fd, const void* data, size_t len) {
  const auto* ptr = static_cast<const uint8_t*>(data);
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = send(fd, ptr, remaining, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(500);
        continue;
      }
      return false;
    }
    if (n == 0) return false;
    ptr += n;
    remaining -= static_cast<size_t>(n);
  }
  return true;
}

bool ReadAll(int fd, void* data, size_t len) {
  auto* ptr = static_cast<uint8_t*>(data);
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = recv(fd, ptr, remaining, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(500);
        continue;
      }
      return false;
    }
    if (n == 0) return false;
    ptr += n;
    remaining -= static_cast<size_t>(n);
  }
  return true;
}

uint32_t NowMonotonicMs() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

// Minimal XKB v1 keymap string so Wayland clients (weston-simple-shm, GTK, Electron/VSCode,
// Antigravity) can initialize xkbcommon state machines without external files on Android.
constexpr const char kMinimalXkbKeymap[] = R"xkb(xkb_keymap {
xkb_keycodes "vkwlf_evdev" {
    minimum = 8;
    maximum = 255;
    <ESC>  = 9;
    <AE01> = 10; <AE02> = 11; <AE03> = 12; <AE04> = 13; <AE05> = 14;
    <AE06> = 15; <AE07> = 16; <AE08> = 17; <AE09> = 18; <AE10> = 19;
    <AE11> = 20; <AE12> = 21; <BKSP> = 22;
    <TAB>  = 23; <AD01> = 24; <AD02> = 25; <AD03> = 26; <AD04> = 27;
    <AD05> = 28; <AD06> = 29; <AD07> = 30; <AD08> = 31; <AD09> = 32;
    <AD10> = 33; <AD11> = 34; <AD12> = 35; <RTRN> = 36;
    <LCTL> = 37; <AC01> = 38; <AC02> = 39; <AC03> = 40; <AC04> = 41;
    <AC05> = 42; <AC06> = 43; <AC07> = 44; <AC08> = 45; <AC09> = 46;
    <AC10> = 47; <AC11> = 48; <TLDE> = 49; <LFSH> = 50; <BKSL> = 51;
    <AB01> = 52; <AB02> = 53; <AB03> = 54; <AB04> = 55; <AB05> = 56;
    <AB06> = 57; <AB07> = 58; <AB08> = 59; <AB09> = 60; <AB10> = 61;
    <RTSH> = 62; <LALT> = 64; <SPCE> = 65; <CAPS> = 66;
    <FK01> = 67; <FK02> = 68; <FK03> = 69; <FK04> = 70; <FK05> = 71;
    <FK06> = 72; <FK07> = 73; <FK08> = 74; <FK09> = 75; <FK10> = 76;
    <FK11> = 95; <FK12> = 96;
    <HOME> = 110; <UP> = 111; <PGUP> = 112; <LEFT> = 113; <RGHT> = 114;
    <END>  = 115; <DOWN> = 116; <PGDN> = 117; <INS> = 118; <DELE> = 119;
    <LWIN> = 133;
    indicator 1 = "Caps Lock";
    indicator 2 = "Num Lock";
    indicator 3 = "Scroll Lock";
};
xkb_types "complete" {
    virtual_modifiers NumLock,Alt,Super,ScrollLock;
    type "ONE_LEVEL" {
        modifiers= none;
        level_name[Level1]= "Any";
    };
    type "TWO_LEVEL" {
        modifiers= Shift;
        map[Shift]= Level2;
        level_name[Level1]= "Base";
        level_name[Level2]= "Shift";
    };
    type "ALPHABETIC" {
        modifiers= Shift+Lock;
        map[Shift]= Level2;
        map[Lock]= Level2;
        level_name[Level1]= "Base";
        level_name[Level2]= "Caps";
    };
    type "KEYPAD" {
        modifiers= Shift+NumLock;
        map[none]= Level1;
        map[Shift]= Level2;
        map[NumLock]= Level2;
        map[Shift+NumLock]= Level1;
        level_name[Level1]= "Base";
        level_name[Level2]= "Number";
    };
    type "FOUR_LEVEL" {
        modifiers= Shift+Alt;
        map[none]= Level1;
        map[Shift]= Level2;
        map[Alt]= Level3;
        map[Shift+Alt]= Level4;
        level_name[Level1]= "Base";
        level_name[Level2]= "Shift";
        level_name[Level3]= "Alt Base";
        level_name[Level4]= "Shift Alt";
    };
};
xkb_compatibility "complete" {
    virtual_modifiers NumLock,Alt,Super,ScrollLock;
    interpret Caps_Lock+AnyOfOrNone(all) {
        action= LockMods(modifiers=Lock);
    };
    interpret Any+AnyOf(all) {
        action= SetMods(modifiers=modMapMods,clearLocks);
    };
    indicator "Caps Lock" {
        whichModState= locked;
        modifiers= Lock;
    };
    indicator "Num Lock" {
        whichModState= locked;
        modifiers= NumLock;
    };
    indicator "Scroll Lock" {
        whichModState= locked;
        modifiers= ScrollLock;
    };
};
xkb_symbols "pc_us" {
    name[group1]="English (US)";
    key <ESC>  { [ Escape ] };
    key <AE01> { [ 1, exclam ] };
    key <AE02> { [ 2, at ] };
    key <AE03> { [ 3, numbersign ] };
    key <AE04> { [ 4, dollar ] };
    key <AE05> { [ 5, percent ] };
    key <AE06> { [ 6, asciicircum ] };
    key <AE07> { [ 7, ampersand ] };
    key <AE08> { [ 8, asterisk ] };
    key <AE09> { [ 9, parenleft ] };
    key <AE10> { [ 0, parenright ] };
    key <AE11> { [ minus, underscore ] };
    key <AE12> { [ equal, plus ] };
    key <BKSP> { [ BackSpace ] };
    key <TAB>  { [ Tab, ISO_Left_Tab ] };
    key <AD01> { [ q, Q ] };
    key <AD02> { [ w, W ] };
    key <AD03> { [ e, E ] };
    key <AD04> { [ r, R ] };
    key <AD05> { [ t, T ] };
    key <AD06> { [ y, Y ] };
    key <AD07> { [ u, U ] };
    key <AD08> { [ i, I ] };
    key <AD09> { [ o, O ] };
    key <AD10> { [ p, P ] };
    key <AD11> { [ bracketleft, braceleft ] };
    key <AD12> { [ bracketright, braceright ] };
    key <RTRN> { [ Return ] };
    key <LCTL> { [ Control_L ] };
    key <AC01> { [ a, A ] };
    key <AC02> { [ s, S ] };
    key <AC03> { [ d, D ] };
    key <AC04> { [ f, F ] };
    key <AC05> { [ g, G ] };
    key <AC06> { [ h, H ] };
    key <AC07> { [ j, J ] };
    key <AC08> { [ k, K ] };
    key <AC09> { [ l, L ] };
    key <AC10> { [ semicolon, colon ] };
    key <AC11> { [ apostrophe, quotedbl ] };
    key <TLDE> { [ grave, asciitilde ] };
    key <LFSH> { [ Shift_L ] };
    key <BKSL> { [ backslash, bar ] };
    key <AB01> { [ z, Z ] };
    key <AB02> { [ x, X ] };
    key <AB03> { [ c, C ] };
    key <AB04> { [ v, V ] };
    key <AB05> { [ b, B ] };
    key <AB06> { [ n, N ] };
    key <AB07> { [ m, M ] };
    key <AB08> { [ comma, less ] };
    key <AB09> { [ period, greater ] };
    key <AB10> { [ slash, question ] };
    key <RTSH> { [ Shift_R ] };
    key <LALT> { [ Alt_L, Meta_L ] };
    key <SPCE> { [ space ] };
    key <CAPS> { [ Caps_Lock ] };
    key <FK01> { [ F1 ] };
    key <FK02> { [ F2 ] };
    key <FK03> { [ F3 ] };
    key <FK04> { [ F4 ] };
    key <FK05> { [ F5 ] };
    key <FK06> { [ F6 ] };
    key <FK07> { [ F7 ] };
    key <FK08> { [ F8 ] };
    key <FK09> { [ F9 ] };
    key <FK10> { [ F10 ] };
    key <FK11> { [ F11 ] };
    key <FK12> { [ F12 ] };
    key <HOME> { [ Home ] };
    key <UP>   { [ Up ] };
    key <PGUP> { [ Prior ] };
    key <LEFT> { [ Left ] };
    key <RGHT> { [ Right ] };
    key <END>  { [ End ] };
    key <DOWN> { [ Down ] };
    key <PGDN> { [ Next ] };
    key <INS>  { [ Insert ] };
    key <DELE> { [ Delete ] };
    key <LWIN> { [ Super_L ] };
    modifier_map Shift   { <LFSH>, <RTSH> };
    modifier_map Lock    { <CAPS> };
    modifier_map Control { <LCTL> };
    modifier_map Mod1    { <LALT> };
    modifier_map Mod4    { <LWIN> };
};
xkb_geometry "pc(pc104)" {};
};
)xkb";

// --- State structures for Wayland resources ---

struct DmaBufBufferResource {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t flags = 0;
  std::vector<DmaBufPlane> planes;
  struct wl_resource* buffer_resource = nullptr;

  ~DmaBufBufferResource() {
    for (auto& p : planes) {
      if (p.fd >= 0) {
        close(p.fd);
        p.fd = -1;
      }
    }
  }
};

struct DmaBufParamsState {
  WaylandServerEngine* engine = nullptr;
  std::vector<DmaBufPlane> planes;

  ~DmaBufParamsState() {
    for (auto& p : planes) {
      if (p.fd >= 0) {
        close(p.fd);
        p.fd = -1;
      }
    }
  }
};

struct XdgPositionerState {
  int32_t width = 256;
  int32_t height = 128;
  int32_t anchor_x = 0;
  int32_t anchor_y = 0;
  int32_t anchor_width = 0;
  int32_t anchor_height = 0;
  uint32_t anchor = 0;
  uint32_t gravity = 0;
  uint32_t constraint_adjustment = 0;
  int32_t offset_x = 0;
  int32_t offset_y = 0;
};

struct SurfaceState {
  WaylandServerEngine* engine = nullptr;
  uint32_t id = 0;
  uint32_t parent_surface_id = 0;
  struct wl_resource* surface_resource = nullptr;
  struct wl_resource* xdg_surface_resource = nullptr;
  struct wl_resource* xdg_toplevel_resource = nullptr;
  struct wl_resource* xdg_popup_resource = nullptr;
  struct wl_resource* subsurface_resource = nullptr;
  struct wl_resource* pending_buffer = nullptr;
  int32_t x = 0;
  int32_t y = 0;
  int32_t buffer_scale = 1;
  int32_t buffer_transform = 0;
  bool configured = false;
  bool is_popup = false;
  bool is_subsurface = false;
  bool app_launch_notified = false;
  bool attach_called = false;
  bool has_committed_buffer = false;
  uint32_t committed_width = 0;
  uint32_t committed_height = 0;
  int32_t popup_width = 0;
  int32_t popup_height = 0;
  // xdg_surface.set_window_geometry: the region of the buffer that is the actual
  // window (CSD clients such as Chromium/GTK draw shadows outside it). Pending
  // values are latched on commit, per protocol.
  bool pending_geometry_set = false;
  int32_t pending_geom_x = 0, pending_geom_y = 0, pending_geom_w = 0, pending_geom_h = 0;
  bool has_geometry = false;
  int32_t geom_x = 0, geom_y = 0, geom_w = 0, geom_h = 0;
  std::string title = "WaylandApp";
  std::string app_id = "wayland.app";
  std::string xwayland_display;
  std::string last_notified_title;
  std::string last_resolved_app_id;
  std::vector<uint8_t> icon_png;
  int icon_frames_sent = 0;
  std::vector<struct wl_resource*> pending_frame_callbacks;
};

void GenericResourceDestroy(struct wl_client* /*client*/, struct wl_resource* resource) {
  wl_resource_destroy(resource);
}

// --- zwp_linux_dmabuf_v1 & zwp_linux_buffer_params_v1 implementation ---

const struct wl_buffer_interface kDmaBufWlBufferImpl = {
    .destroy = GenericResourceDestroy,
};

void OnDmaBufWlBufferDestroy(struct wl_resource* resource) {
  auto* dmabuf = static_cast<DmaBufBufferResource*>(wl_resource_get_user_data(resource));
  delete dmabuf;
}

void DmaBufParamsAdd(struct wl_client* /*client*/,
                     struct wl_resource* resource,
                     int32_t fd,
                     uint32_t plane_idx,
                     uint32_t offset,
                     uint32_t stride,
                     uint32_t modifier_hi,
                     uint32_t modifier_lo) {
  auto* params = static_cast<DmaBufParamsState*>(wl_resource_get_user_data(resource));
  if (!params) {
    if (fd >= 0) close(fd);
    return;
  }
  uint64_t mod = (static_cast<uint64_t>(modifier_hi) << 32) | modifier_lo;
  if (params->planes.size() <= plane_idx) {
    params->planes.resize(plane_idx + 1);
  }
  if (params->planes[plane_idx].fd >= 0) {
    close(params->planes[plane_idx].fd);
  }
  params->planes[plane_idx] = DmaBufPlane{
      .fd = fd,
      .offset = offset,
      .stride = stride,
      .modifier = mod,
  };
  LOGV(kTag, "zwp_linux_buffer_params_v1.add: plane=%u fd=%d offset=%u stride=%u mod=0x%llx",
       plane_idx, fd, offset, stride, static_cast<unsigned long long>(mod));
}

struct wl_resource* CreateDmaBufBufferObject(struct wl_client* client,
                                             struct wl_resource* params_resource,
                                             uint32_t buffer_id,
                                             int32_t width,
                                             int32_t height,
                                             uint32_t format,
                                             uint32_t flags) {
  auto* params = static_cast<DmaBufParamsState*>(wl_resource_get_user_data(params_resource));
  if (!params || params->planes.empty() || width <= 0 || height <= 0) {
    zwp_linux_buffer_params_v1_send_failed(params_resource);
    return nullptr;
  }

  auto* buf = new DmaBufBufferResource();
  buf->width = static_cast<uint32_t>(width);
  buf->height = static_cast<uint32_t>(height);
  buf->format = format;
  buf->flags = flags;
  buf->planes = std::move(params->planes);
  params->planes.clear();

  struct wl_resource* buf_res =
      wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
  if (!buf_res) {
    delete buf;
    wl_resource_post_no_memory(params_resource);
    return nullptr;
  }

  buf->buffer_resource = buf_res;
  wl_resource_set_implementation(buf_res, &kDmaBufWlBufferImpl, buf, OnDmaBufWlBufferDestroy);

  LOGI(kTag,
       "Created zero-copy DMA-BUF wl_buffer (%ux%u, format=0x%08x, fd=%d, stride=%u)",
       buf->width, buf->height, buf->format, buf->planes[0].fd, buf->planes[0].stride);
  return buf_res;
}

void DmaBufParamsCreate(struct wl_client* client,
                        struct wl_resource* resource,
                        int32_t width,
                        int32_t height,
                        uint32_t format,
                        uint32_t flags) {
  struct wl_resource* buf_res =
      CreateDmaBufBufferObject(client, resource, 0, width, height, format, flags);
  if (buf_res) {
    zwp_linux_buffer_params_v1_send_created(resource, buf_res);
  }
}

void DmaBufParamsCreateImmed(struct wl_client* client,
                             struct wl_resource* resource,
                             uint32_t buffer_id,
                             int32_t width,
                             int32_t height,
                             uint32_t format,
                             uint32_t flags) {
  CreateDmaBufBufferObject(client, resource, buffer_id, width, height, format, flags);
}

const struct zwp_linux_buffer_params_v1_interface kDmaBufParamsImpl = {
    .destroy = GenericResourceDestroy,
    .add = DmaBufParamsAdd,
    .create = DmaBufParamsCreate,
    .create_immed = DmaBufParamsCreateImmed,
};

void OnDmaBufParamsDestroy(struct wl_resource* resource) {
  auto* params = static_cast<DmaBufParamsState*>(wl_resource_get_user_data(resource));
  delete params;
}

void DmaBufCreateParams(struct wl_client* client,
                        struct wl_resource* resource,
                        uint32_t params_id) {
  auto* engine = static_cast<WaylandServerEngine*>(wl_resource_get_user_data(resource));
  struct wl_resource* params_res = wl_resource_create(
      client, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(resource), params_id);
  if (!params_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  auto* state = new DmaBufParamsState();
  state->engine = engine;
  wl_resource_set_implementation(params_res, &kDmaBufParamsImpl, state, OnDmaBufParamsDestroy);
}

const struct zwp_linux_dmabuf_feedback_v1_interface kDmaBufFeedbackImpl = {
    .destroy = GenericResourceDestroy,
};

void SendDmaBufFeedbackEvents(struct wl_resource* fb_res) {
  if (!fb_res) return;

  struct stat st {};
  dev_t main_dev = 0;
  if (stat("/dev/dri/renderD128", &st) == 0 && st.st_rdev != 0) {
    main_dev = st.st_rdev;
  } else if (stat("/dev/dri/card0", &st) == 0 && st.st_rdev != 0) {
    main_dev = st.st_rdev;
  }

  struct FormatTableEntry {
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
  };
  constexpr uint64_t kDrmFormatModLinear = 0ULL;
  constexpr uint64_t kDrmFormatModInvalid = 0x00ffffffffffffffULL;
  const uint32_t formats[] = {kDrmFormatARGB8888, kDrmFormatXRGB8888, kDrmFormatABGR8888,
                              kDrmFormatXBGR8888};
  FormatTableEntry entries[8]{};
  for (size_t i = 0; i < 4; ++i) {
    entries[i * 2 + 0] = {formats[i], 0u, kDrmFormatModLinear};
    entries[i * 2 + 1] = {formats[i], 0u, kDrmFormatModInvalid};
  }

  int table_fd = memfd_create("vkwlf-dmabuf-feedback-table", MFD_CLOEXEC);
  if (table_fd >= 0) {
    if (write(table_fd, entries, sizeof(entries)) == static_cast<ssize_t>(sizeof(entries))) {
      lseek(table_fd, 0, SEEK_SET);
      zwp_linux_dmabuf_feedback_v1_send_format_table(fb_res, table_fd, sizeof(entries));
    }
    close(table_fd);
  }

  struct wl_array dev_arr;
  wl_array_init(&dev_arr);
  if (auto* p = static_cast<dev_t*>(wl_array_add(&dev_arr, sizeof(dev_t)))) {
    *p = main_dev;
  }
  zwp_linux_dmabuf_feedback_v1_send_main_device(fb_res, &dev_arr);
  zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(fb_res, &dev_arr);
  zwp_linux_dmabuf_feedback_v1_send_tranche_flags(
      fb_res, ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT);

  struct wl_array idx_arr;
  wl_array_init(&idx_arr);
  for (uint16_t idx = 0; idx < 8; ++idx) {
    if (auto* pi = static_cast<uint16_t*>(wl_array_add(&idx_arr, sizeof(uint16_t)))) {
      *pi = idx;
    }
  }
  zwp_linux_dmabuf_feedback_v1_send_tranche_formats(fb_res, &idx_arr);
  wl_array_release(&idx_arr);
  wl_array_release(&dev_arr);

  zwp_linux_dmabuf_feedback_v1_send_tranche_done(fb_res);
  zwp_linux_dmabuf_feedback_v1_send_done(fb_res);
}

void DmaBufGetDefaultFeedback(struct wl_client* client,
                              struct wl_resource* resource,
                              uint32_t id) {
  struct wl_resource* fb_res = wl_resource_create(
      client, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(resource), id);
  if (!fb_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  wl_resource_set_implementation(fb_res, &kDmaBufFeedbackImpl, nullptr, nullptr);
  SendDmaBufFeedbackEvents(fb_res);
}

void DmaBufGetSurfaceFeedback(struct wl_client* client,
                              struct wl_resource* resource,
                              uint32_t id,
                              struct wl_resource* /*surface*/) {
  DmaBufGetDefaultFeedback(client, resource, id);
}

const struct zwp_linux_dmabuf_v1_interface kLinuxDmaBufImpl = {
    .destroy = GenericResourceDestroy,
    .create_params = DmaBufCreateParams,
    .get_default_feedback = DmaBufGetDefaultFeedback,
    .get_surface_feedback = DmaBufGetSurfaceFeedback,
};

void BindLinuxDmaBuf(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto* engine = static_cast<WaylandServerEngine*>(data);
  uint32_t bound_ver = std::min<uint32_t>(version, 4);
  struct wl_resource* res =
      wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, bound_ver, id);
  if (!res) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(res, &kLinuxDmaBufImpl, engine, nullptr);

  if (bound_ver < 4) {
    const uint32_t formats[] = {kDrmFormatARGB8888, kDrmFormatXRGB8888, kDrmFormatABGR8888,
                                kDrmFormatXBGR8888};
    for (uint32_t fmt : formats) {
      zwp_linux_dmabuf_v1_send_format(res, fmt);
      if (bound_ver >= 3) {
        zwp_linux_dmabuf_v1_send_modifier(res, fmt, 0, 0);
        zwp_linux_dmabuf_v1_send_modifier(res, fmt, 0x00ffffffu, 0xffffffffu);
      }
    }
  }
}

// --- wl_region implementation ---

void RegionAdd(struct wl_client* /*client*/,
               struct wl_resource* /*resource*/,
               int32_t /*x*/,
               int32_t /*y*/,
               int32_t /*width*/,
               int32_t /*height*/) {}

void RegionSubtract(struct wl_client* /*client*/,
                    struct wl_resource* /*resource*/,
                    int32_t /*x*/,
                    int32_t /*y*/,
                    int32_t /*width*/,
                    int32_t /*height*/) {}

const struct wl_region_interface kRegionImpl = {
    .destroy = GenericResourceDestroy,
    .add = RegionAdd,
    .subtract = RegionSubtract,
};

// --- wl_surface implementation ---

void SurfaceDestroy(struct wl_client* /*client*/, struct wl_resource* resource) {
  wl_resource_destroy(resource);
}

void SurfaceAttach(struct wl_client* /*client*/,
                   struct wl_resource* resource,
                   struct wl_resource* buffer,
                   int32_t x,
                   int32_t y) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (!surf) return;
  surf->pending_buffer = buffer;
  surf->attach_called = true;
  if (x != 0 || y != 0) {
    surf->x += x;
    surf->y += y;
  }
}

void SurfaceDamage(struct wl_client* /*client*/,
                   struct wl_resource* /*resource*/,
                   int32_t /*x*/,
                   int32_t /*y*/,
                   int32_t /*width*/,
                   int32_t /*height*/) {}

void SurfaceFrame(struct wl_client* client,
                  struct wl_resource* resource,
                  uint32_t callback_id) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  struct wl_resource* cb_res =
      wl_resource_create(client, &wl_callback_interface, 1, callback_id);
  if (!cb_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  wl_resource_set_implementation(cb_res, nullptr, nullptr, nullptr);
  if (surf) {
    surf->pending_frame_callbacks.push_back(cb_res);
  }
}

void SurfaceSetOpaqueRegion(struct wl_client* /*client*/,
                            struct wl_resource* /*resource*/,
                            struct wl_resource* /*region*/) {}

void SurfaceSetInputRegion(struct wl_client* /*client*/,
                           struct wl_resource* /*resource*/,
                           struct wl_resource* /*region*/) {}

void SurfaceCommit(struct wl_client* /*client*/, struct wl_resource* resource) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (!surf || !surf->engine) return;

  WaylandServerEngine* engine = surf->engine;

  // If this surface is an xdg_surface that hasn't been configured yet, send initial configure.
  if (surf->xdg_surface_resource && !surf->configured) {
    surf->configured = true;
    if (surf->xdg_toplevel_resource) {
      struct wl_array states;
      wl_array_init(&states);
      auto* act = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
      if (act) *act = XDG_TOPLEVEL_STATE_ACTIVATED;
      auto* max_st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
      if (max_st) *max_st = XDG_TOPLEVEL_STATE_MAXIMIZED;

      xdg_toplevel_send_configure(surf->xdg_toplevel_resource,
                                  static_cast<int32_t>(engine->config().display_width),
                                  static_cast<int32_t>(engine->config().display_height), &states);
      wl_array_release(&states);
      xdg_surface_send_configure(surf->xdg_surface_resource, engine->NextSerial());
      LOGD(kTag, "Sent xdg_toplevel.configure(%ux%u) + xdg_surface.configure(serial) for surface %u",
           engine->config().display_width, engine->config().display_height, surf->id);
    } else if (surf->xdg_popup_resource) {
      xdg_popup_send_configure(surf->xdg_popup_resource, surf->x, surf->y,
                               surf->popup_width > 0 ? surf->popup_width : 256,
                               surf->popup_height > 0 ? surf->popup_height : 128);
      xdg_surface_send_configure(surf->xdg_surface_resource, engine->NextSerial());
      LOGD(kTag, "Sent xdg_popup.configure(%d,%d %dx%d) for popup surface %u", surf->x, surf->y,
           surf->popup_width, surf->popup_height, surf->id);
    }
  }

  if (surf->xdg_toplevel_resource && (!surf->has_committed_buffer || !engine->FocusedSurface())) {
    engine->SetFocusedSurface(surf->surface_resource);
  }

  // Ignore cursor surfaces or surfaces without an xdg_toplevel / xdg_popup / wl_subsurface role.
  const bool has_drawable_role = (surf->xdg_toplevel_resource != nullptr) ||
                                 (surf->xdg_popup_resource != nullptr) || surf->is_subsurface;
  if (!has_drawable_role) {
    if (surf->pending_buffer) {
      wl_buffer_send_release(surf->pending_buffer);
      surf->pending_buffer = nullptr;
    }
    const uint32_t now_ms = NowMonotonicMs();
    for (struct wl_resource* cb : surf->pending_frame_callbacks) {
      wl_callback_send_done(cb, now_ms);
      wl_resource_destroy(cb);
    }
    surf->pending_frame_callbacks.clear();
    return;
  }

  if (surf->pending_buffer) {
    struct wl_resource* buf_res = surf->pending_buffer;
    surf->pending_buffer = nullptr;
    surf->attach_called = false;
    surf->has_committed_buffer = true;

    CommittedFrame frame;
    frame.surface_id = surf->id;
    frame.parent_surface_id = surf->parent_surface_id;
    frame.frame_number = engine->NextFrameNumber();
    frame.title = surf->title;
    frame.app_id = surf->app_id;
    frame.x = surf->x;
    frame.y = surf->y;
    frame.is_popup = surf->is_popup;
    frame.is_subsurface = surf->is_subsurface;

    if (surf->xdg_toplevel_resource != nullptr) {
      if (surf->xwayland_display.empty() && surf->title.rfind("Xwayland on :", 0) == 0) {
        surf->xwayland_display = TrimAscii(surf->title.substr(12));
      }
      if (!surf->xwayland_display.empty() &&
          (surf->app_id.empty() || surf->app_id == "org.freedesktop.Xwayland" ||
           surf->title.rfind("Xwayland on :", 0) == 0)) {
        std::string resolved_x_title;
        std::string resolved_x_app_id;
        if (ResolveXwaylandClientMetadata(surf->xwayland_display, &resolved_x_title,
                                          &resolved_x_app_id)) {
          surf->title = resolved_x_title;
          surf->app_id = resolved_x_app_id;
          frame.title = surf->title;
          frame.app_id = surf->app_id;
        }
      }

      if (surf->icon_png.empty() || surf->app_id != surf->last_resolved_app_id) {
        auto resolved = ResolveLinuxAppIconPng(wl_resource_get_client(resource), surf->app_id,
                                               surf->title);
        if (!resolved.empty()) {
          surf->icon_png = std::move(resolved);
          surf->last_resolved_app_id = surf->app_id;
          surf->icon_frames_sent = 0;
        }
      }
      if (!surf->icon_png.empty()) {
        frame.icon_png = surf->icon_png;
      }
      const bool is_unresolved_xwayland =
          (!surf->xwayland_display.empty() && surf->app_id == "org.freedesktop.Xwayland");
      const bool title_changed = (surf->title != surf->last_notified_title);
      if (!is_unresolved_xwayland &&
          (!surf->app_launch_notified || title_changed || surf->icon_frames_sent < 4)) {
        surf->app_launch_notified = true;
        surf->last_notified_title = surf->title;
        if (!surf->icon_png.empty()) {
          surf->icon_frames_sent++;
        }
        if (title_changed || surf->icon_frames_sent <= 1) {
          engine->NotifyAppLaunched(surf->id, surf->title, surf->app_id, surf->icon_png);
        }
      }
    }

    // Latch xdg_surface.set_window_geometry (double-buffered state) for both wl_shm and dma_buf.
    if (surf->pending_geometry_set) {
      surf->has_geometry = true;
      surf->geom_x = surf->pending_geom_x;
      surf->geom_y = surf->pending_geom_y;
      surf->geom_w = surf->pending_geom_w;
      surf->geom_h = surf->pending_geom_h;
      surf->pending_geometry_set = false;
    }

    struct wl_shm_buffer* shm_buf = wl_shm_buffer_get(buf_res);
    if (shm_buf) {
      frame.buffer_type = BufferType::kDmaBuf;
      frame.width = static_cast<uint32_t>(wl_shm_buffer_get_width(shm_buf));
      frame.height = static_cast<uint32_t>(wl_shm_buffer_get_height(shm_buf));
      frame.stride = static_cast<uint32_t>(wl_shm_buffer_get_stride(shm_buf));
      frame.format = wl_shm_buffer_get_format(shm_buf);

      // Crop to the window geometry when it is a strict sub-rectangle of the buffer:
      // CSD clients (Chromium, GTK) leave the area outside it undefined (shadows or
      // stale pool memory), and it must not be presented or counted as window size.
      int32_t crop_x = 0, crop_y = 0;
      uint32_t crop_w = frame.width, crop_h = frame.height;
      if (surf->has_geometry && surf->geom_w > 0 && surf->geom_h > 0 &&
          surf->geom_x >= 0 && surf->geom_y >= 0 &&
          static_cast<uint32_t>(surf->geom_x + surf->geom_w) <= frame.width &&
          static_cast<uint32_t>(surf->geom_y + surf->geom_h) <= frame.height &&
          (static_cast<uint32_t>(surf->geom_w) != frame.width ||
           static_cast<uint32_t>(surf->geom_h) != frame.height)) {
        crop_x = surf->geom_x;
        crop_y = surf->geom_y;
        crop_w = static_cast<uint32_t>(surf->geom_w);
        crop_h = static_cast<uint32_t>(surf->geom_h);
      }
      if (surf->xdg_toplevel_resource != nullptr && surf->parent_surface_id == 0) {
        engine->RecordSurfaceGeometry(surf->id, crop_x, crop_y, crop_w, crop_h);
        if (crop_w >= (engine->config().display_width * 3) / 4 &&
            crop_h >= (engine->config().display_height * 3) / 4) {
          engine->SetToplevelGeometry(crop_x, crop_y, crop_w, crop_h);
        }
      }

      wl_shm_buffer_begin_access(shm_buf);
      const auto* raw_data = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm_buf));
      if (raw_data) {
        if (crop_w == frame.width && crop_h == frame.height) {
          size_t byte_len = static_cast<size_t>(frame.stride) * frame.height;
          if (byte_len > 0) frame.shm_pixels.assign(raw_data, raw_data + byte_len);
        } else {
          const uint32_t bpp = 4;  // all advertised wl_shm formats are 32bpp
          const uint32_t dst_stride = crop_w * bpp;
          frame.shm_pixels.resize(static_cast<size_t>(dst_stride) * crop_h);
          for (uint32_t row = 0; row < crop_h; ++row) {
            const uint8_t* src =
                raw_data + static_cast<size_t>(crop_y + row) * frame.stride + static_cast<size_t>(crop_x) * bpp;
            std::memcpy(frame.shm_pixels.data() + static_cast<size_t>(row) * dst_stride, src, dst_stride);
          }
          frame.width = crop_w;
          frame.height = crop_h;
          frame.stride = dst_stride;
        }
      }
      wl_shm_buffer_end_access(shm_buf);

#ifndef __ANDROID__
      // Export the buffer into a memfd DMA-BUF descriptor on the Linux guest so guest VkRenderer
      // imports it via VK_KHR_external_memory_fd / VK_EXT_external_memory_host with shm_uploads=0.
      if (!frame.shm_pixels.empty()) {
        int mfd = memfd_create("vkwlf-guest-dmabuf", MFD_CLOEXEC);
        if (mfd >= 0 && ftruncate(mfd, static_cast<off_t>(frame.shm_pixels.size())) == 0) {
          void* mapped = mmap(nullptr, frame.shm_pixels.size(), PROT_READ | PROT_WRITE, MAP_SHARED,
                              mfd, 0);
          if (mapped != MAP_FAILED) {
            std::memcpy(mapped, frame.shm_pixels.data(), frame.shm_pixels.size());
            munmap(mapped, frame.shm_pixels.size());
            frame.dmabuf_fd = mfd;
            frame.planes.push_back(DmaBufPlane{
                .fd = dup(mfd),
                .offset = 0,
                .stride = frame.stride,
                .modifier = 0,
            });
          } else {
            close(mfd);
          }
        } else if (mfd >= 0) {
          close(mfd);
        }
      }
#endif

      LOGV(kTag,
           "wl_surface.commit [host_buffer]: surface=%u frame=%llu title='%s' size=%ux%u stride=%u format=0x%x%s",
           frame.surface_id, static_cast<unsigned long long>(frame.frame_number),
           frame.title.c_str(), frame.width, frame.height, frame.stride, frame.format,
           (crop_x || crop_y) ? " (cropped to window geometry)" : "");

      surf->committed_width = frame.width;
      surf->committed_height = frame.height;
      engine->PushCommittedFrame(std::move(frame));
      wl_buffer_send_release(buf_res);
    } else if (wl_resource_instance_of(buf_res, &wl_buffer_interface, &kDmaBufWlBufferImpl)) {
      // Zero-copy zwp_linux_dmabuf_v1 buffer path
      auto* dmabuf = static_cast<DmaBufBufferResource*>(wl_resource_get_user_data(buf_res));
      if (dmabuf && !dmabuf->planes.empty() && dmabuf->planes[0].fd >= 0) {
        frame.buffer_type = BufferType::kDmaBuf;
        frame.width = dmabuf->width;
        frame.height = dmabuf->height;
        frame.stride = dmabuf->planes[0].stride;
        frame.format = dmabuf->format;
        frame.drm_modifier = dmabuf->planes[0].modifier;

        int32_t crop_x = 0, crop_y = 0;
        uint32_t crop_w = frame.width, crop_h = frame.height;
        if (surf->has_geometry && surf->geom_w > 0 && surf->geom_h > 0 &&
            surf->geom_x >= 0 && surf->geom_y >= 0 &&
            static_cast<uint32_t>(surf->geom_x + surf->geom_w) <= frame.width &&
            static_cast<uint32_t>(surf->geom_y + surf->geom_h) <= frame.height &&
            (static_cast<uint32_t>(surf->geom_w) != frame.width ||
             static_cast<uint32_t>(surf->geom_h) != frame.height)) {
          crop_x = surf->geom_x;
          crop_y = surf->geom_y;
          crop_w = static_cast<uint32_t>(surf->geom_w);
          crop_h = static_cast<uint32_t>(surf->geom_h);
        }
        if (surf->xdg_toplevel_resource != nullptr && surf->parent_surface_id == 0) {
          engine->RecordSurfaceGeometry(surf->id, crop_x, crop_y, crop_w, crop_h);
          if (crop_w >= (engine->config().display_width * 3) / 4 &&
              crop_h >= (engine->config().display_height * 3) / 4) {
            engine->SetToplevelGeometry(crop_x, crop_y, crop_w, crop_h);
          }
        }

        const bool is_ahb_handle =
            (dmabuf->planes.size() >= 2 &&
             dmabuf->planes.back().modifier == 0x414842484E444C00ULL);
        if (!is_ahb_handle && (crop_w != frame.width || crop_h != frame.height)) {
          const uint32_t plane_offset = dmabuf->planes[0].offset;
          const size_t src_map_len =
              static_cast<size_t>(plane_offset) + static_cast<size_t>(frame.stride) * frame.height;
          void* src_map = mmap(nullptr, src_map_len, PROT_READ, MAP_SHARED, dmabuf->planes[0].fd, 0);
          if (src_map != MAP_FAILED) {
            const uint32_t dst_stride = crop_w * 4;
            const size_t dst_bytes = static_cast<size_t>(dst_stride) * crop_h;
            int mfd = memfd_create("vkwlf-cropped-dmabuf", MFD_CLOEXEC);
            if (mfd >= 0 && ftruncate(mfd, static_cast<off_t>(dst_bytes)) == 0) {
              void* dst_map = mmap(nullptr, dst_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
              if (dst_map != MAP_FAILED) {
                const auto* src_base = static_cast<const uint8_t*>(src_map) + plane_offset;
                auto* dst_base = static_cast<uint8_t*>(dst_map);
                for (uint32_t row = 0; row < crop_h; ++row) {
                  std::memcpy(dst_base + static_cast<size_t>(row) * dst_stride,
                              src_base + static_cast<size_t>(crop_y + row) * frame.stride +
                                  static_cast<size_t>(crop_x) * 4,
                              dst_stride);
                }
                munmap(dst_map, dst_bytes);
                frame.width = crop_w;
                frame.height = crop_h;
                frame.stride = dst_stride;
                frame.dmabuf_fd = mfd;
                frame.planes.push_back(DmaBufPlane{
                    .fd = dup(mfd),
                    .offset = 0,
                    .stride = dst_stride,
                    .modifier = 0,
                });
              } else {
                close(mfd);
              }
            } else if (mfd >= 0) {
              close(mfd);
            }
            munmap(src_map, src_map_len);
          }
        }

        if (frame.dmabuf_fd < 0) {
          frame.dmabuf_fd = dup(dmabuf->planes[0].fd);
          for (const auto& p : dmabuf->planes) {
            frame.planes.push_back(DmaBufPlane{
                .fd = (p.fd >= 0) ? dup(p.fd) : -1,
                .offset = p.offset,
                .stride = p.stride,
                .modifier = p.modifier,
            });
          }
        }

        LOGV(kTag,
             "wl_surface.commit [dma_buf]: surface=%u frame=%llu title='%s' size=%ux%u stride=%u fourcc=0x%08x fd=%d",
             frame.surface_id, static_cast<unsigned long long>(frame.frame_number),
             frame.title.c_str(), frame.width, frame.height, frame.stride, frame.format,
             frame.dmabuf_fd);

        surf->committed_width = frame.width;
        surf->committed_height = frame.height;
        engine->PushCommittedFrame(std::move(frame));
      }
      wl_buffer_send_release(buf_res);
    }
  } else if (surf->attach_called) {
    // Client attached a NULL wl_buffer and committed -> unmap surface
    surf->attach_called = false;
    if (surf->has_committed_buffer) {
      surf->has_committed_buffer = false;
      engine->OnSurfaceUnmappedOrDestroyed(surf->surface_resource, surf->id, true);
    }
  }

  // Fire pending wl_surface.frame callbacks so client continues rendering smoothly at VSync pace.
  const uint32_t now_ms = NowMonotonicMs();
  for (struct wl_resource* cb : surf->pending_frame_callbacks) {
    wl_callback_send_done(cb, now_ms);
    wl_resource_destroy(cb);
  }
  surf->pending_frame_callbacks.clear();
}

void SurfaceSetBufferTransform(struct wl_client* /*client*/,
                               struct wl_resource* resource,
                               int32_t transform) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf) surf->buffer_transform = transform;
}

void SurfaceSetBufferScale(struct wl_client* /*client*/,
                           struct wl_resource* resource,
                           int32_t scale) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf) surf->buffer_scale = scale;
}

void SurfaceDamageBuffer(struct wl_client* /*client*/,
                         struct wl_resource* /*resource*/,
                         int32_t /*x*/,
                         int32_t /*y*/,
                         int32_t /*width*/,
                         int32_t /*height*/) {}

void SurfaceOffset(struct wl_client* /*client*/,
                   struct wl_resource* /*resource*/,
                   int32_t /*x*/,
                   int32_t /*y*/) {}

const struct wl_surface_interface kSurfaceImpl = {
    .destroy = SurfaceDestroy,
    .attach = SurfaceAttach,
    .damage = SurfaceDamage,
    .frame = SurfaceFrame,
    .set_opaque_region = SurfaceSetOpaqueRegion,
    .set_input_region = SurfaceSetInputRegion,
    .commit = SurfaceCommit,
    .set_buffer_transform = SurfaceSetBufferTransform,
    .set_buffer_scale = SurfaceSetBufferScale,
    .damage_buffer = SurfaceDamageBuffer,
    .offset = SurfaceOffset,
};

void OnSurfaceResourceDestroy(struct wl_resource* resource) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf) {
    if (surf->xdg_toplevel_resource) {
      if (surf->engine) {
        surf->engine->UnregisterToplevelSurface(surf->xdg_toplevel_resource);
      }
      wl_resource_set_user_data(surf->xdg_toplevel_resource, nullptr);
      surf->xdg_toplevel_resource = nullptr;
    }
    if (surf->xdg_popup_resource) {
      wl_resource_set_user_data(surf->xdg_popup_resource, nullptr);
      surf->xdg_popup_resource = nullptr;
    }
    if (surf->xdg_surface_resource) {
      wl_resource_set_user_data(surf->xdg_surface_resource, nullptr);
      surf->xdg_surface_resource = nullptr;
    }
    if (surf->subsurface_resource) {
      wl_resource_set_user_data(surf->subsurface_resource, nullptr);
      surf->subsurface_resource = nullptr;
    }
    if (surf->engine) {
      surf->engine->OnSurfaceUnmappedOrDestroyed(resource, surf->id, surf->has_committed_buffer);
      surf->has_committed_buffer = false;
    }
    delete surf;
  }
}

// --- wl_compositor implementation ---

void CompositorCreateSurface(struct wl_client* client,
                             struct wl_resource* resource,
                             uint32_t id) {
  auto* engine = static_cast<WaylandServerEngine*>(wl_resource_get_user_data(resource));
  struct wl_resource* surf_res =
      wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
  if (!surf_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  static std::atomic<uint32_t> next_surface_id{1};
  auto* surf = new SurfaceState();
  surf->engine = engine;
  surf->id = next_surface_id++;
  surf->surface_resource = surf_res;
  wl_resource_set_implementation(surf_res, &kSurfaceImpl, surf, OnSurfaceResourceDestroy);
  LOGD(kTag, "wl_compositor.create_surface -> id=%u (wl_id=%u)", surf->id, id);
}

void CompositorCreateRegion(struct wl_client* client,
                            struct wl_resource* resource,
                            uint32_t id) {
  struct wl_resource* reg_res =
      wl_resource_create(client, &wl_region_interface, wl_resource_get_version(resource), id);
  if (!reg_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  wl_resource_set_implementation(reg_res, &kRegionImpl, nullptr, nullptr);
}

const struct wl_compositor_interface kCompositorImpl = {
    .create_surface = CompositorCreateSurface,
    .create_region = CompositorCreateRegion,
};

void BindCompositor(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto* engine = static_cast<WaylandServerEngine*>(data);
  struct wl_resource* res =
      wl_resource_create(client, &wl_compositor_interface, std::min<uint32_t>(version, 6), id);
  if (!res) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(res, &kCompositorImpl, engine, nullptr);
}

// --- wl_subcompositor & wl_subsurface implementation ---

void SubsurfaceSetPosition(struct wl_client* /*client*/,
                           struct wl_resource* resource,
                           int32_t x,
                           int32_t y) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf) {
    surf->x = x;
    surf->y = y;
  }
}

void SubsurfacePlaceAbove(struct wl_client* /*client*/,
                          struct wl_resource* /*resource*/,
                          struct wl_resource* /*sibling*/) {}

void SubsurfacePlaceBelow(struct wl_client* /*client*/,
                          struct wl_resource* /*resource*/,
                          struct wl_resource* /*sibling*/) {}

void SubsurfaceSetSync(struct wl_client* /*client*/, struct wl_resource* /*resource*/) {}

void SubsurfaceSetDesync(struct wl_client* /*client*/, struct wl_resource* /*resource*/) {}

const struct wl_subsurface_interface kSubsurfaceImpl = {
    .destroy = GenericResourceDestroy,
    .set_position = SubsurfaceSetPosition,
    .place_above = SubsurfacePlaceAbove,
    .place_below = SubsurfacePlaceBelow,
    .set_sync = SubsurfaceSetSync,
    .set_desync = SubsurfaceSetDesync,
};

void OnSubsurfaceDestroy(struct wl_resource* resource) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf) {
    surf->subsurface_resource = nullptr;
  }
}

void SubcompositorGetSubsurface(struct wl_client* client,
                                struct wl_resource* resource,
                                uint32_t id,
                                struct wl_resource* surface,
                                struct wl_resource* parent) {
  struct wl_resource* sub_res =
      wl_resource_create(client, &wl_subsurface_interface, wl_resource_get_version(resource), id);
  if (!sub_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  auto* surf = surface ? static_cast<SurfaceState*>(wl_resource_get_user_data(surface)) : nullptr;
  auto* parent_surf =
      parent ? static_cast<SurfaceState*>(wl_resource_get_user_data(parent)) : nullptr;
  if (surf) {
    surf->is_subsurface = true;
    surf->subsurface_resource = sub_res;
    if (parent_surf) {
      surf->parent_surface_id = parent_surf->id;
    }
  }
  wl_resource_set_implementation(sub_res, &kSubsurfaceImpl, surf, OnSubsurfaceDestroy);
}

const struct wl_subcompositor_interface kSubcompositorImpl = {
    .destroy = GenericResourceDestroy,
    .get_subsurface = SubcompositorGetSubsurface,
};

void BindSubcompositor(struct wl_client* client, void* data, uint32_t /*version*/, uint32_t id) {
  struct wl_resource* res = wl_resource_create(client, &wl_subcompositor_interface, 1, id);
  if (!res) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(res, &kSubcompositorImpl, data, nullptr);
}

// --- wl_data_device_manager (v3) clipboard implementation (Required by Electron/VSCode) ---

void DataSourceOffer(struct wl_client* /*client*/,
                     struct wl_resource* /*resource*/,
                     const char* mime_type) {
  LOGV(kTag, "wl_data_source.offer: mime_type='%s'", mime_type ? mime_type : "");
}

void DataSourceSetActions(struct wl_client* /*client*/,
                          struct wl_resource* /*resource*/,
                          uint32_t /*dnd_actions*/) {}

const struct wl_data_source_interface kDataSourceImpl = {
    .offer = DataSourceOffer,
    .destroy = GenericResourceDestroy,
    .set_actions = DataSourceSetActions,
};

void DataDeviceStartDrag(struct wl_client* /*client*/,
                         struct wl_resource* /*resource*/,
                         struct wl_resource* /*source*/,
                         struct wl_resource* /*origin*/,
                         struct wl_resource* /*icon*/,
                         uint32_t /*serial*/) {}

void DataDeviceSetSelection(struct wl_client* /*client*/,
                            struct wl_resource* /*resource*/,
                            struct wl_resource* /*source*/,
                            uint32_t serial) {
  LOGD(kTag, "wl_data_device.set_selection (serial=%u)", serial);
}

const struct wl_data_device_interface kDataDeviceImpl = {
    .start_drag = DataDeviceStartDrag,
    .set_selection = DataDeviceSetSelection,
    .release = GenericResourceDestroy,
};

void DataDeviceManagerCreateDataSource(struct wl_client* client,
                                       struct wl_resource* resource,
                                       uint32_t id) {
  struct wl_resource* res =
      wl_resource_create(client, &wl_data_source_interface, wl_resource_get_version(resource), id);
  if (!res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  wl_resource_set_implementation(res, &kDataSourceImpl, nullptr, nullptr);
}

void DataDeviceManagerGetDataDevice(struct wl_client* client,
                                    struct wl_resource* resource,
                                    uint32_t id,
                                    struct wl_resource* /*seat*/) {
  struct wl_resource* res =
      wl_resource_create(client, &wl_data_device_interface, wl_resource_get_version(resource), id);
  if (!res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  wl_resource_set_implementation(res, &kDataDeviceImpl, nullptr, nullptr);
}

const struct wl_data_device_manager_interface kDataDeviceManagerImpl = {
    .create_data_source = DataDeviceManagerCreateDataSource,
    .get_data_device = DataDeviceManagerGetDataDevice,
};

void BindDataDeviceManager(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
  struct wl_resource* res = wl_resource_create(
      client, &wl_data_device_manager_interface, std::min<uint32_t>(version, 3), id);
  if (!res) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(res, &kDataDeviceManagerImpl, data, nullptr);
}

// --- xdg_positioner, xdg_popup, xdg_toplevel, xdg_surface, xdg_wm_base implementation ---

void XdgPositionerSetSize(struct wl_client* /*client*/,
                          struct wl_resource* resource,
                          int32_t width,
                          int32_t height) {
  auto* pos = static_cast<XdgPositionerState*>(wl_resource_get_user_data(resource));
  if (pos) {
    pos->width = std::max<int32_t>(1, width);
    pos->height = std::max<int32_t>(1, height);
  }
}

void XdgPositionerSetAnchorRect(struct wl_client* /*client*/,
                                struct wl_resource* resource,
                                int32_t x,
                                int32_t y,
                                int32_t width,
                                int32_t height) {
  auto* pos = static_cast<XdgPositionerState*>(wl_resource_get_user_data(resource));
  if (pos) {
    pos->anchor_x = x;
    pos->anchor_y = y;
    pos->anchor_width = width;
    pos->anchor_height = height;
  }
}

void XdgPositionerSetAnchor(struct wl_client* /*client*/,
                            struct wl_resource* resource,
                            uint32_t anchor) {
  auto* pos = static_cast<XdgPositionerState*>(wl_resource_get_user_data(resource));
  if (pos) pos->anchor = anchor;
}

void XdgPositionerSetGravity(struct wl_client* /*client*/,
                             struct wl_resource* resource,
                             uint32_t gravity) {
  auto* pos = static_cast<XdgPositionerState*>(wl_resource_get_user_data(resource));
  if (pos) pos->gravity = gravity;
}

void XdgPositionerSetConstraintAdjustment(struct wl_client* /*client*/,
                                          struct wl_resource* resource,
                                          uint32_t constraint_adjustment) {
  auto* pos = static_cast<XdgPositionerState*>(wl_resource_get_user_data(resource));
  if (pos) pos->constraint_adjustment = constraint_adjustment;
}

void XdgPositionerSetOffset(struct wl_client* /*client*/,
                            struct wl_resource* resource,
                            int32_t x,
                            int32_t y) {
  auto* pos = static_cast<XdgPositionerState*>(wl_resource_get_user_data(resource));
  if (pos) {
    pos->offset_x = x;
    pos->offset_y = y;
  }
}

void XdgPositionerSetReactive(struct wl_client* /*client*/, struct wl_resource* /*resource*/) {}

void XdgPositionerSetParentSize(struct wl_client* /*client*/,
                                struct wl_resource* /*resource*/,
                                int32_t /*parent_width*/,
                                int32_t /*parent_height*/) {}

void XdgPositionerSetParentConfigure(struct wl_client* /*client*/,
                                     struct wl_resource* /*resource*/,
                                     uint32_t /*serial*/) {}

const struct xdg_positioner_interface kXdgPositionerImpl = {
    .destroy = GenericResourceDestroy,
    .set_size = XdgPositionerSetSize,
    .set_anchor_rect = XdgPositionerSetAnchorRect,
    .set_anchor = XdgPositionerSetAnchor,
    .set_gravity = XdgPositionerSetGravity,
    .set_constraint_adjustment = XdgPositionerSetConstraintAdjustment,
    .set_offset = XdgPositionerSetOffset,
    .set_reactive = XdgPositionerSetReactive,
    .set_parent_size = XdgPositionerSetParentSize,
    .set_parent_configure = XdgPositionerSetParentConfigure,
};

void OnXdgPositionerDestroy(struct wl_resource* resource) {
  delete static_cast<XdgPositionerState*>(wl_resource_get_user_data(resource));
}

void XdgPopupGrab(struct wl_client* /*client*/,
                  struct wl_resource* /*resource*/,
                  struct wl_resource* /*seat*/,
                  uint32_t /*serial*/) {}

void XdgPopupReposition(struct wl_client* /*client*/,
                        struct wl_resource* resource,
                        struct wl_resource* positioner,
                        uint32_t token) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  auto* pos =
      positioner ? static_cast<XdgPositionerState*>(wl_resource_get_user_data(positioner)) : nullptr;
  if (surf && pos && surf->engine) {
    surf->x = pos->anchor_x + pos->offset_x;
    surf->y = pos->anchor_y + pos->anchor_height + pos->offset_y;
    surf->popup_width = pos->width;
    surf->popup_height = pos->height;
    xdg_popup_send_repositioned(resource, token);
    xdg_popup_send_configure(resource, surf->x, surf->y, surf->popup_width, surf->popup_height);
    if (surf->xdg_surface_resource) {
      xdg_surface_send_configure(surf->xdg_surface_resource, surf->engine->NextSerial());
    }
  }
}

const struct xdg_popup_interface kXdgPopupImpl = {
    .destroy = GenericResourceDestroy,
    .grab = XdgPopupGrab,
    .reposition = XdgPopupReposition,
};

void OnXdgPopupDestroy(struct wl_resource* resource) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf) {
    surf->xdg_popup_resource = nullptr;
    if (surf->engine && surf->has_committed_buffer) {
      surf->has_committed_buffer = false;
      surf->engine->OnSurfaceUnmappedOrDestroyed(surf->surface_resource, surf->id, true);
    }
  }
}

void XdgToplevelSetParent(struct wl_client* /*client*/,
                          struct wl_resource* resource,
                          struct wl_resource* parent) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  auto* parent_surf =
      parent ? static_cast<SurfaceState*>(wl_resource_get_user_data(parent)) : nullptr;
  if (surf && parent_surf) {
    surf->parent_surface_id = parent_surf->id;
  }
}

void XdgToplevelSetTitle(struct wl_client* /*client*/,
                         struct wl_resource* resource,
                         const char* title) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf && title) {
    surf->title = title;
    LOGI(kTag, "xdg_toplevel.set_title: surface=%u title='%s'", surf->id, surf->title.c_str());
  }
}

void XdgToplevelSetAppId(struct wl_client* /*client*/,
                         struct wl_resource* resource,
                         const char* app_id) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf && app_id) {
    surf->app_id = app_id;
    LOGI(kTag, "xdg_toplevel.set_app_id: surface=%u app_id='%s'", surf->id, surf->app_id.c_str());
  }
}

void XdgToplevelShowWindowMenu(struct wl_client* /*client*/,
                               struct wl_resource* /*resource*/,
                               struct wl_resource* /*seat*/,
                               uint32_t /*serial*/,
                               int32_t /*x*/,
                               int32_t /*y*/) {}

void XdgToplevelMove(struct wl_client* /*client*/,
                     struct wl_resource* /*resource*/,
                     struct wl_resource* /*seat*/,
                     uint32_t /*serial*/) {}

void XdgToplevelResize(struct wl_client* /*client*/,
                       struct wl_resource* /*resource*/,
                       struct wl_resource* /*seat*/,
                       uint32_t /*serial*/,
                       uint32_t /*edges*/) {}

void XdgToplevelSetMaxSize(struct wl_client* /*client*/,
                           struct wl_resource* /*resource*/,
                           int32_t /*width*/,
                           int32_t /*height*/) {}

void XdgToplevelSetMinSize(struct wl_client* /*client*/,
                           struct wl_resource* /*resource*/,
                           int32_t /*width*/,
                           int32_t /*height*/) {}

void XdgToplevelSetMaximized(struct wl_client* /*client*/, struct wl_resource* /*resource*/) {}
void XdgToplevelUnsetMaximized(struct wl_client* /*client*/, struct wl_resource* /*resource*/) {}

void XdgToplevelSetFullscreen(struct wl_client* /*client*/,
                              struct wl_resource* /*resource*/,
                              struct wl_resource* /*output*/) {}

void XdgToplevelUnsetFullscreen(struct wl_client* /*client*/, struct wl_resource* /*resource*/) {}
void XdgToplevelSetMinimized(struct wl_client* /*client*/, struct wl_resource* /*resource*/) {}

const struct xdg_toplevel_interface kXdgToplevelImpl = {
    .destroy = GenericResourceDestroy,
    .set_parent = XdgToplevelSetParent,
    .set_title = XdgToplevelSetTitle,
    .set_app_id = XdgToplevelSetAppId,
    .show_window_menu = XdgToplevelShowWindowMenu,
    .move = XdgToplevelMove,
    .resize = XdgToplevelResize,
    .set_max_size = XdgToplevelSetMaxSize,
    .set_min_size = XdgToplevelSetMinSize,
    .set_maximized = XdgToplevelSetMaximized,
    .unset_maximized = XdgToplevelUnsetMaximized,
    .set_fullscreen = XdgToplevelSetFullscreen,
    .unset_fullscreen = XdgToplevelUnsetFullscreen,
    .set_minimized = XdgToplevelSetMinimized,
};

void OnXdgToplevelDestroy(struct wl_resource* resource) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf) {
    if (surf->engine) {
      surf->engine->UnregisterToplevelSurface(resource);
      if (surf->has_committed_buffer) {
        surf->has_committed_buffer = false;
        surf->engine->OnSurfaceUnmappedOrDestroyed(surf->surface_resource, surf->id, true);
      }
    }
    surf->xdg_toplevel_resource = nullptr;
  }
}

void XdgSurfaceGetToplevel(struct wl_client* client,
                           struct wl_resource* resource,
                           uint32_t id) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  struct wl_resource* top_res =
      wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
  if (!top_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  wl_resource_set_implementation(top_res, &kXdgToplevelImpl, surf, OnXdgToplevelDestroy);
  if (surf && surf->engine) {
    surf->xdg_toplevel_resource = top_res;
    surf->configured = true;
    surf->engine->RegisterToplevelSurface(surf->surface_resource, resource, top_res);

    struct wl_array states;
    wl_array_init(&states);
    auto* act = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    if (act) *act = XDG_TOPLEVEL_STATE_ACTIVATED;
    auto* max_st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    if (max_st) *max_st = XDG_TOPLEVEL_STATE_MAXIMIZED;

    xdg_toplevel_send_configure(top_res,
                                static_cast<int32_t>(surf->engine->config().display_width),
                                static_cast<int32_t>(surf->engine->config().display_height),
                                &states);
    wl_array_release(&states);

    uint32_t serial = surf->engine->NextSerial();
    xdg_surface_send_configure(resource, serial);
    LOGD(kTag,
         "Sent xdg_toplevel.configure(%ux%u) + xdg_surface.configure(serial=%u) for surface %u",
         surf->engine->config().display_width, surf->engine->config().display_height, serial,
         surf->id);
  }
}

void XdgSurfaceGetPopup(struct wl_client* client,
                        struct wl_resource* resource,
                        uint32_t id,
                        struct wl_resource* parent,
                        struct wl_resource* positioner) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  auto* parent_surf =
      parent ? static_cast<SurfaceState*>(wl_resource_get_user_data(parent)) : nullptr;
  auto* pos =
      positioner ? static_cast<XdgPositionerState*>(wl_resource_get_user_data(positioner)) : nullptr;

  struct wl_resource* popup_res =
      wl_resource_create(client, &xdg_popup_interface, wl_resource_get_version(resource), id);
  if (!popup_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  wl_resource_set_implementation(popup_res, &kXdgPopupImpl, surf, OnXdgPopupDestroy);

  if (surf && surf->engine) {
    surf->xdg_popup_resource = popup_res;
    surf->is_popup = true;
    surf->configured = true;
    if (parent_surf) {
      surf->parent_surface_id = parent_surf->id;
    }
    if (pos) {
      surf->x = pos->anchor_x + pos->offset_x;
      surf->y = pos->anchor_y + pos->anchor_height + pos->offset_y;
      surf->popup_width = pos->width;
      surf->popup_height = pos->height;
    } else {
      surf->popup_width = 256;
      surf->popup_height = 128;
    }

    xdg_popup_send_configure(popup_res, surf->x, surf->y, surf->popup_width, surf->popup_height);
    uint32_t serial = surf->engine->NextSerial();
    xdg_surface_send_configure(resource, serial);
    LOGI(kTag,
         "Created xdg_popup for surface %u (parent=%u, pos=%d,%d, size=%dx%d, serial=%u)",
         surf->id, surf->parent_surface_id, surf->x, surf->y, surf->popup_width,
         surf->popup_height, serial);
  }
}

void XdgSurfaceSetWindowGeometry(struct wl_client* /*client*/,
                                 struct wl_resource* resource,
                                 int32_t x,
                                 int32_t y,
                                 int32_t width,
                                 int32_t height) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (!surf || width <= 0 || height <= 0) return;
  surf->pending_geometry_set = true;
  surf->pending_geom_x = x;
  surf->pending_geom_y = y;
  surf->pending_geom_w = width;
  surf->pending_geom_h = height;
  LOGV(kTag, "xdg_surface.set_window_geometry: surface=%u %dx%d+%d+%d", surf->id, width, height,
       x, y);
}

void XdgSurfaceAckConfigure(struct wl_client* /*client*/,
                            struct wl_resource* resource,
                            uint32_t serial) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf) {
    surf->configured = true;
    LOGV(kTag, "xdg_surface.ack_configure: surface=%u serial=%u", surf->id, serial);
  }
}

const struct xdg_surface_interface kXdgSurfaceImpl = {
    .destroy = GenericResourceDestroy,
    .get_toplevel = XdgSurfaceGetToplevel,
    .get_popup = XdgSurfaceGetPopup,
    .set_window_geometry = XdgSurfaceSetWindowGeometry,
    .ack_configure = XdgSurfaceAckConfigure,
};

void OnXdgSurfaceDestroy(struct wl_resource* resource) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
  if (surf) {
    surf->xdg_surface_resource = nullptr;
  }
}

void XdgWmBaseCreatePositioner(struct wl_client* client,
                               struct wl_resource* resource,
                               uint32_t id) {
  struct wl_resource* pos_res =
      wl_resource_create(client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
  if (!pos_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  auto* pos = new XdgPositionerState();
  wl_resource_set_implementation(pos_res, &kXdgPositionerImpl, pos, OnXdgPositionerDestroy);
}

void XdgWmBaseGetXdgSurface(struct wl_client* client,
                            struct wl_resource* resource,
                            uint32_t id,
                            struct wl_resource* surface_resource) {
  auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(surface_resource));
  struct wl_resource* xdg_surf_res =
      wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
  if (!xdg_surf_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  if (surf) {
    surf->xdg_surface_resource = xdg_surf_res;
  }
  wl_resource_set_implementation(xdg_surf_res, &kXdgSurfaceImpl, surf, OnXdgSurfaceDestroy);
}

void XdgWmBasePong(struct wl_client* /*client*/,
                   struct wl_resource* /*resource*/,
                   uint32_t serial) {
  LOGV(kTag, "xdg_wm_base.pong received (serial=%u)", serial);
}

const struct xdg_wm_base_interface kXdgWmBaseImpl = {
    .destroy = GenericResourceDestroy,
    .create_positioner = XdgWmBaseCreatePositioner,
    .get_xdg_surface = XdgWmBaseGetXdgSurface,
    .pong = XdgWmBasePong,
};

void BindXdgWmBase(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto* engine = static_cast<WaylandServerEngine*>(data);
  struct wl_resource* res =
      wl_resource_create(client, &xdg_wm_base_interface, std::min<uint32_t>(version, 3), id);
  if (!res) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(res, &kXdgWmBaseImpl, engine, nullptr);
  xdg_wm_base_send_ping(res, engine->NextSerial());
}

// --- wl_output implementation ---

const struct wl_output_interface kOutputImpl = {
    .release = GenericResourceDestroy,
};

void BindOutput(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto* engine = static_cast<WaylandServerEngine*>(data);
  uint32_t bound_ver = std::min<uint32_t>(version, 3);
  struct wl_resource* res = wl_resource_create(client, &wl_output_interface, bound_ver, id);
  if (!res) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(res, &kOutputImpl, engine, nullptr);

  wl_output_send_geometry(res, 0, 0, 340, 190, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Google",
                          "VkWlfApp-AVF-Display", WL_OUTPUT_TRANSFORM_NORMAL);
  wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                      static_cast<int32_t>(engine->config().display_width),
                      static_cast<int32_t>(engine->config().display_height),
                      static_cast<int32_t>(engine->config().refresh_mhz));
  if (bound_ver >= 2) {
    wl_output_send_scale(res, 1);
    wl_output_send_done(res);
  }
}

// --- wl_seat / wl_pointer / wl_keyboard implementation ---

void PointerSetCursor(struct wl_client* /*client*/,
                      struct wl_resource* /*resource*/,
                      uint32_t /*serial*/,
                      struct wl_resource* /*surface*/,
                      int32_t /*hotspot_x*/,
                      int32_t /*hotspot_y*/) {}

const struct wl_pointer_interface kPointerImpl = {
    .set_cursor = PointerSetCursor,
    .release = GenericResourceDestroy,
};

void OnPointerResourceDestroy(struct wl_resource* resource) {
  auto* engine = static_cast<WaylandServerEngine*>(wl_resource_get_user_data(resource));
  if (engine) {
    engine->UnregisterClientPointer(resource);
  }
}

const struct wl_keyboard_interface kKeyboardImpl = {
    .release = GenericResourceDestroy,
};

void OnKeyboardResourceDestroy(struct wl_resource* resource) {
  auto* engine = static_cast<WaylandServerEngine*>(wl_resource_get_user_data(resource));
  if (engine) {
    engine->UnregisterClientKeyboard(resource);
  }
}

void SeatGetPointer(struct wl_client* client, struct wl_resource* resource, uint32_t id) {
  auto* engine = static_cast<WaylandServerEngine*>(wl_resource_get_user_data(resource));
  struct wl_resource* ptr_res =
      wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
  if (!ptr_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  wl_resource_set_implementation(ptr_res, &kPointerImpl, engine, OnPointerResourceDestroy);
  if (engine) {
    engine->RegisterClientPointer(ptr_res);
  }
}

void SeatGetKeyboard(struct wl_client* client, struct wl_resource* resource, uint32_t id) {
  auto* engine = static_cast<WaylandServerEngine*>(wl_resource_get_user_data(resource));
  struct wl_resource* kbd_res =
      wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
  if (!kbd_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  wl_resource_set_implementation(kbd_res, &kKeyboardImpl, engine, OnKeyboardResourceDestroy);
  if (engine) {
    engine->RegisterClientKeyboard(kbd_res);
  }
}

void SeatGetTouch(struct wl_client* client, struct wl_resource* resource, uint32_t id) {
  struct wl_resource* touch_res =
      wl_resource_create(client, &wl_touch_interface, wl_resource_get_version(resource), id);
  if (!touch_res) {
    wl_resource_post_no_memory(resource);
    return;
  }
  static const struct wl_touch_interface kTouchImpl = {
      .release = GenericResourceDestroy,
  };
  wl_resource_set_implementation(touch_res, &kTouchImpl, nullptr, nullptr);
}

const struct wl_seat_interface kSeatImpl = {
    .get_pointer = SeatGetPointer,
    .get_keyboard = SeatGetKeyboard,
    .get_touch = SeatGetTouch,
    .release = GenericResourceDestroy,
};

void BindSeat(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto* engine = static_cast<WaylandServerEngine*>(data);
  uint32_t bound_ver = std::min<uint32_t>(version, 5);
  struct wl_resource* res = wl_resource_create(client, &wl_seat_interface, bound_ver, id);
  if (!res) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(res, &kSeatImpl, engine, nullptr);
  wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
  if (bound_ver >= 2) {
    wl_seat_send_name(res, "vkwlf-android-seat0");
  }
}

}  // namespace

// --- WaylandServerEngine class implementation ---

WaylandServerEngine::WaylandServerEngine(const Config& config) : config_(config) {}

WaylandServerEngine::~WaylandServerEngine() { Stop(); }

void WaylandServerEngine::SetAppLaunchedCallback(AppLaunchedCallback cb) {
  std::lock_guard<std::mutex> lock(app_cb_mutex_);
  app_launched_cb_ = std::move(cb);
}

void WaylandServerEngine::SetAppCatalogCallback(AppCatalogCallback cb) {
  std::lock_guard<std::mutex> lock(app_cb_mutex_);
  app_catalog_cb_ = std::move(cb);
}

std::vector<GuestAppCatalogEntry> WaylandServerEngine::GetGuestAppCatalog() const {
  std::lock_guard<std::mutex> lock(app_catalog_mutex_);
  return guest_app_catalog_;
}

void WaylandServerEngine::NotifyAppCatalogUpdated(std::vector<GuestAppCatalogEntry>&& entries) {
  std::vector<GuestAppCatalogEntry> snapshot;
  {
    std::lock_guard<std::mutex> lock(app_catalog_mutex_);
    guest_app_catalog_ = std::move(entries);
    snapshot = guest_app_catalog_;
  }
  AppCatalogCallback cb;
  {
    std::lock_guard<std::mutex> lock(app_cb_mutex_);
    cb = app_catalog_cb_;
  }
  if (cb) {
    cb(snapshot);
  }
}

void WaylandServerEngine::RequestGuestAppLaunch(const std::string& launch_spec) {
  if (launch_spec.empty()) return;
  EnqueueInputCommand(InputCommand{
      .type = InputCommand::Type::kLaunchApp,
      .launch_spec = launch_spec,
  });
}

void WaylandServerEngine::NotifyAppLaunched(uint32_t surface_id,
                                            const std::string& title,
                                            const std::string& app_id,
                                            const std::vector<uint8_t>& icon_png) {
  AppLaunchedCallback cb;
  {
    std::lock_guard<std::mutex> lock(app_cb_mutex_);
    cb = app_launched_cb_;
  }
  if (cb) {
    cb(surface_id, title, app_id, icon_png);
  }
}

std::vector<GuestAppCatalogEntry> WaylandServerEngine::DiscoverLocalGuestAppCatalog() {
  std::vector<GuestAppCatalogEntry> catalog;
  std::unordered_map<std::string, size_t> seen_ids;
  const auto data_dirs = GetXdgDataDirs();

  auto is_ignored_desktop = [](const DesktopEntryInfo& info) -> bool {
    if (info.no_display || info.terminal) return true;
    std::string low_id = ToLowerAscii(info.desktop_id);
    std::string low_name = ToLowerAscii(info.name);
    if (low_id.find("url-handler") != std::string::npos ||
        low_id.find("urlhandler") != std::string::npos ||
        low_id.find("url_forwarder") != std::string::npos ||
        low_id.find("teamvieweruri") != std::string::npos ||
        low_id.find("xwayland") != std::string::npos ||
        low_id.rfind("python", 0) == 0 ||
        low_id == "vim") {
      return true;
    }
    return false;
  };

  for (const auto& base_dir : data_dirs) {
    std::string app_dir = base_dir + "/applications";
    DIR* d = opendir(app_dir.c_str());
    if (!d) continue;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
      std::string fname = ent->d_name;
      if (fname.size() <= 8 || fname.substr(fname.size() - 8) != ".desktop") continue;
      DesktopEntryInfo info;
      if (!ParseDesktopFile(app_dir + "/" + fname, fname, &info)) continue;
      if (is_ignored_desktop(info)) continue;

      std::string low_id = ToLowerAscii(info.desktop_id);
      if (seen_ids.find(low_id) != seen_ids.end()) continue;

      GuestAppCatalogEntry entry;
      entry.desktop_id = info.desktop_id;
      entry.name = info.name;
      entry.comment = info.comment;
      entry.exec = info.exec;
      entry.installed = true;
      if (!info.icon.empty()) {
        ResolveIconValueToPng(info.icon, &entry.icon_png);
      }
      if (entry.icon_png.empty()) {
        ResolveIconValueToPng(info.desktop_id, &entry.icon_png);
      }
      seen_ids[low_id] = catalog.size();
      catalog.push_back(std::move(entry));
    }
    closedir(d);
  }

  struct CuratedCandidate {
    const char* desktop_id;
    const char* name;
    const char* comment;
    const char* bin_check;
    const char* exec_cmd;
    const char* apt_package;
    const char* icon_name;
  };
  static const CuratedCandidate kCurated[] = {
      {"firefox-esr", "Firefox ESR", "Mozilla Firefox Web Browser (Wayland)",
       "/usr/bin/firefox-esr", "firefox-esr", "firefox-esr", "firefox-esr"},
      {"gimp", "GIMP", "GNU Image Manipulation Program",
       "/usr/bin/gimp", "gimp", "gimp", "gimp"},
      {"com.microsoft.VSCode", "Visual Studio Code", "Code Editing. Redefined.",
       "/usr/share/code/code", "/usr/share/code/code", "code", "vscode"},
      {"antigravity", "Antigravity", "Antigravity IDE (Wayland)",
       "/usr/share/antigravity/antigravity", "/usr/share/antigravity/antigravity", "antigravity", "antigravity"},
      {"com.teamviewer.TeamViewer", "TeamViewer", "Remote Control Solution (Xwayland)",
       "/opt/teamviewer/tv_bin/TeamViewer", "/opt/teamviewer/tv_bin/TeamViewer", "teamviewer", "TeamViewer"},
      {"chromium", "Chromium", "Chromium Web Browser (Wayland)",
       "/usr/bin/chromium", "chromium", "chromium", "chromium"},
      {"vlc", "VLC Media Player", "Multimedia Player & Streamer",
       "/usr/bin/vlc", "vlc", "vlc", "vlc"},
      {"libreoffice", "LibreOffice", "Office Productivity Suite",
       "/usr/bin/libreoffice", "libreoffice", "libreoffice", "libreoffice-main"},
      {"foot", "Foot Terminal", "Fast, Lightweight Wayland Terminal Emulator",
       "/usr/bin/foot", "foot", "foot", "foot"},
      {"weston-terminal", "Weston Terminal", "Wayland Terminal Client",
       "/usr/bin/weston-terminal", "weston-terminal", "weston", "utilities-terminal"},
      {"org.gnome.Nautilus", "Files (Nautilus)", "GNOME File Manager",
       "/usr/bin/nautilus", "nautilus", "nautilus", "org.gnome.Nautilus"},
      {"inkscape", "Inkscape", "Vector Graphics Editor",
       "/usr/bin/inkscape", "inkscape", "inkscape", "inkscape"},
  };

  for (const auto& c : kCurated) {
    std::string low_id = ToLowerAscii(c.desktop_id);
    bool already_present = (seen_ids.find(low_id) != seen_ids.end());
    if (!already_present) {
      for (const auto& existing : catalog) {
        if (ToLowerAscii(existing.name).find(ToLowerAscii(c.apt_package)) != std::string::npos ||
            ToLowerAscii(existing.desktop_id).find(ToLowerAscii(c.apt_package)) != std::string::npos) {
          already_present = true;
          break;
        }
      }
    }
    if (already_present) continue;

    struct stat st {};
    bool bin_installed = (stat(c.bin_check, &st) == 0 && (st.st_mode & S_IXUSR));
    GuestAppCatalogEntry entry;
    entry.desktop_id = c.desktop_id;
    entry.name = c.name;
    entry.comment = c.comment;
    entry.exec = c.exec_cmd;
    entry.apt_package = c.apt_package;
    entry.installed = bin_installed;
    ResolveIconValueToPng(c.icon_name, &entry.icon_png);
    catalog.push_back(std::move(entry));
  }

  std::sort(catalog.begin(), catalog.end(),
            [](const GuestAppCatalogEntry& a, const GuestAppCatalogEntry& b) {
              if (a.installed != b.installed) return a.installed > b.installed;
              return ToLowerAscii(a.name) < ToLowerAscii(b.name);
            });
  return catalog;
}

bool WaylandServerEngine::SendAppCatalogOverBridgeSocket(
    int sock_fd, const std::vector<GuestAppCatalogEntry>& entries) {
  if (sock_fd < 0) return false;
  std::vector<uint8_t> buf;
  auto append_u8 = [&](uint8_t v) { buf.push_back(v); };
  auto append_u16 = [&](uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  };
  auto append_u32 = [&](uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  };
  auto append_str16 = [&](const std::string& s) {
    uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.size(), 4096));
    append_u16(len);
    buf.insert(buf.end(), s.begin(), s.begin() + len);
  };

  uint32_t count = static_cast<uint32_t>(std::min<size_t>(entries.size(), 256));
  append_u32(count);
  for (uint32_t i = 0; i < count; ++i) {
    const auto& e = entries[i];
    append_str16(e.desktop_id);
    append_str16(e.name);
    append_str16(e.comment);
    append_str16(e.exec);
    append_str16(e.apt_package);
    append_u8(e.installed ? 1 : 0);
    uint32_t icon_len = static_cast<uint32_t>(std::min<size_t>(e.icon_png.size(), 512 * 1024));
    append_u32(icon_len);
    if (icon_len > 0) {
      buf.insert(buf.end(), e.icon_png.begin(), e.icon_png.begin() + icon_len);
    }
  }

  BridgeFrameHeader hdr{};
  hdr.magic = kBridgeFrameMagic;
  hdr.surface_id = 0xFFFFFFFFu;
  std::string tag = "__VKWLF_APP_CATALOG__";
  hdr.app_id_len = static_cast<uint16_t>(tag.size());
  hdr.payload_bytes = static_cast<uint32_t>(buf.size());

  static std::mutex send_mu;
  std::lock_guard<std::mutex> lock(send_mu);
  if (!WriteAll(sock_fd, &hdr, sizeof(hdr))) return false;
  if (!WriteAll(sock_fd, tag.data(), hdr.app_id_len)) return false;
  if (!buf.empty() && !WriteAll(sock_fd, buf.data(), buf.size())) return false;
  return true;
}

bool WaylandServerEngine::Initialize() {
  display_ = wl_display_create();
  if (!display_) {
    LOGE(kTag, "Failed to create wl_display");
    return false;
  }

  event_loop_ = wl_display_get_event_loop(display_);
  if (!event_loop_) {
    LOGE(kTag, "Failed to get wl_event_loop from wl_display");
    return false;
  }

  // Create self-pipe for thread-safe input event & shutdown wakeups
  if (pipe2(wakeup_pipe_, O_CLOEXEC | O_NONBLOCK) != 0) {
    LOGE(kTag, "Failed to create wakeup pipe: %s", strerror(errno));
    return false;
  }

  wakeup_source_ = wl_event_loop_add_fd(event_loop_, wakeup_pipe_[0], WL_EVENT_READABLE,
                                        &WaylandServerEngine::OnWakeupFdEvent, this);

  // Initialize built-in wl_shm and advertise ARGB8888 / XRGB8888
  wl_display_init_shm(display_);
  wl_display_add_shm_format(display_, WL_SHM_FORMAT_ARGB8888);
  wl_display_add_shm_format(display_, WL_SHM_FORMAT_XRGB8888);

  // Register global Wayland protocols required by weston-simple-shm, GTK, and Electron (VS Code-class IDEs)
  wl_global_create(display_, &wl_compositor_interface, 5, this, BindCompositor);
  wl_global_create(display_, &wl_subcompositor_interface, 1, this, BindSubcompositor);
  wl_global_create(display_, &wl_data_device_manager_interface, 3, this, BindDataDeviceManager);
  wl_global_create(display_, &xdg_wm_base_interface, 3, this, BindXdgWmBase);
  wl_global_create(display_, &zwp_linux_dmabuf_v1_interface, 4, this, BindLinuxDmaBuf);
  wl_global_create(display_, &wl_output_interface, 3, this, BindOutput);
  wl_global_create(display_, &wl_seat_interface, 5, this, BindSeat);

  // Bind the Wayland Unix domain socket.
  std::vector<std::string> candidate_paths = {config_.socket_path};
  if (config_.socket_path == "/data/local/tmp/wayland-0") {
    if (const char* xdg_rt = std::getenv("XDG_RUNTIME_DIR"); xdg_rt && xdg_rt[0] != '\0') {
      candidate_paths.push_back(std::string(xdg_rt) + "/wayland-0");
    }
    candidate_paths.push_back("/data/user/10/com.google.example.vkwlf/cache/wayland-0");
    candidate_paths.push_back("/data/user/0/com.google.example.vkwlf/cache/wayland-0");
    candidate_paths.push_back("/data/data/com.google.example.vkwlf/cache/wayland-0");
    candidate_paths.push_back("/data/user/10/com.android.virtualization.terminal/cache/wayland-0");
    candidate_paths.push_back("/data/data/com.android.virtualization.terminal/cache/wayland-0");
    candidate_paths.push_back("/tmp/wayland-0");
  }

  bool socket_bound = false;
  for (const auto& candidate : candidate_paths) {
    std::string lock_path = candidate + ".lock";
    unlink(candidate.c_str());
    unlink(lock_path.c_str());

    if (wl_display_add_socket(display_, candidate.c_str()) == 0) {
      chmod(candidate.c_str(), 0777);
      bound_socket_path_ = candidate;
      socket_bound = true;
      LOGI(kTag, "Wayland display initialized; Unix socket bound at %s",
           bound_socket_path_.c_str());
      break;
    }
  }

  if (!socket_bound) {
    LOGW(kTag, "Could not bind Wayland Unix socket at %s; relying on VSOCK/TCP bridge listeners",
         config_.socket_path.c_str());
  }

  if (config_.vsock_port > 0) {
    SetupVsockListener(config_.vsock_port);
  }
  if (config_.tcp_port > 0) {
    SetupTcpListener(config_.tcp_port);
  }

  return true;
}

bool WaylandServerEngine::SetupVsockListener(int port) {
  for (int try_port : {port, port + 1}) {
    vsock_fd_ = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (vsock_fd_ < 0) {
      LOGW(kTag, "AF_VSOCK not available on this kernel (%s); using Unix/TCP socket",
           strerror(errno));
      return false;
    }

    struct sockaddr_vm addr {};
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_ANY;
    addr.svm_port = static_cast<unsigned int>(try_port);

    if (bind(vsock_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0 &&
        listen(vsock_fd_, 8) == 0) {
      vsock_source_ = wl_event_loop_add_fd(event_loop_, vsock_fd_, WL_EVENT_READABLE,
                                           &WaylandServerEngine::OnVsockAcceptEvent, this);
      LOGI(kTag, "Bound AF_VSOCK Wayland Bridge listener on CID_ANY:%d", try_port);
      return true;
    }
    LOGW(kTag, "Could not bind/listen AF_VSOCK port %d (%s)", try_port, strerror(errno));
    close(vsock_fd_);
    vsock_fd_ = -1;
  }
  return false;
}

bool WaylandServerEngine::SetupTcpListener(int port) {
  for (int try_port : {port, port + 1}) {
    tcp_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (tcp_fd_ < 0) {
      return false;
    }
    int opt = 1;
    setsockopt(tcp_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(try_port));

    if (bind(tcp_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0 &&
        listen(tcp_fd_, 8) == 0) {
      tcp_source_ = wl_event_loop_add_fd(event_loop_, tcp_fd_, WL_EVENT_READABLE,
                                         &WaylandServerEngine::OnTcpAcceptEvent, this);
      LOGI(kTag, "Bound TCP Wayland Bridge listener on 0.0.0.0:%d", try_port);
      return true;
    }
    LOGW(kTag, "Could not bind/listen TCP bridge port %d (%s)", try_port, strerror(errno));
    close(tcp_fd_);
    tcp_fd_ = -1;
  }
  return false;
}

void WaylandServerEngine::AcceptBridgeClient(int listen_fd, const char* transport_name) {
  int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (client_fd < 0) return;

  LOGI(kTag, "Accepted %s guest bridge connection (fd=%d)", transport_name, client_fd);
  if (config_.display_width > 0 && config_.display_height > 0) {
    BridgeInputPacket init_pkt{};
    init_pkt.magic = kBridgeInputMagic;
    init_pkt.type = static_cast<uint32_t>(InputCommand::Type::kDisplayResize);
    init_pkt.x = static_cast<double>(config_.display_width);
    init_pkt.y = static_cast<double>(config_.display_height);
    init_pkt.mods_latched = config_.display_width;
    init_pkt.group = config_.display_height;
    WriteAll(client_fd, &init_pkt, sizeof(init_pkt));
  }
  auto bc = std::make_unique<BridgeClient>();
  bc->fd = client_fd;
  bc->engine = this;
  bc->source = wl_event_loop_add_fd(event_loop_, client_fd, WL_EVENT_READABLE,
                                    &WaylandServerEngine::OnBridgeClientDataEvent, bc.get());
  std::lock_guard<std::mutex> lock(bridge_clients_mutex_);
  bridge_clients_.push_back(std::move(bc));
}

int WaylandServerEngine::OnVsockAcceptEvent(int fd, uint32_t /*mask*/, void* data) {
  auto* self = static_cast<WaylandServerEngine*>(data);
  self->AcceptBridgeClient(fd, "AF_VSOCK");
  return 0;
}

int WaylandServerEngine::OnTcpAcceptEvent(int fd, uint32_t /*mask*/, void* data) {
  auto* self = static_cast<WaylandServerEngine*>(data);
  self->AcceptBridgeClient(fd, "TCP");
  return 0;
}

int WaylandServerEngine::OnBridgeClientDataEvent(int fd, uint32_t /*mask*/, void* data) {
  auto* bc = static_cast<BridgeClient*>(data);
  if (!bc || !bc->engine) return 0;
  WaylandServerEngine* engine = bc->engine;

  auto disconnect_client = [&]() -> int {
    LOGI(kTag, "Guest bridge client disconnected (fd=%d)", fd);
    if (bc->source) {
      wl_event_source_remove(bc->source);
      bc->source = nullptr;
    }
    close(fd);
    bc->fd = -1;
    return 0;
  };

  BridgeFrameHeader hdr{};
  if (!ReadAll(fd, &hdr, sizeof(hdr)) || hdr.magic != kBridgeFrameMagic) {
    return disconnect_client();
  }

  std::string title(hdr.title_len, '\0');
  if (hdr.title_len > 0 && !ReadAll(fd, title.data(), hdr.title_len)) {
    return disconnect_client();
  }
  std::string app_id(hdr.app_id_len, '\0');
  if (hdr.app_id_len > 0 && !ReadAll(fd, app_id.data(), hdr.app_id_len)) {
    return disconnect_client();
  }
  std::vector<uint8_t> icon_png(hdr.icon_png_len);
  if (hdr.icon_png_len > 0 && !ReadAll(fd, icon_png.data(), hdr.icon_png_len)) {
    return disconnect_client();
  }
  std::vector<uint8_t> payload(hdr.payload_bytes);
  if (hdr.payload_bytes > 0 && !ReadAll(fd, payload.data(), hdr.payload_bytes)) {
    return disconnect_client();
  }

  if (hdr.surface_id == 0xFFFFFFFFu && app_id == "__VKWLF_APP_CATALOG__") {
    size_t pos = 0;
    auto read_u8 = [&](uint8_t* out) -> bool {
      if (pos + 1 > payload.size()) return false;
      *out = payload[pos++];
      return true;
    };
    auto read_u16 = [&](uint16_t* out) -> bool {
      if (pos + 2 > payload.size()) return false;
      *out = static_cast<uint16_t>(payload[pos]) |
             (static_cast<uint16_t>(payload[pos + 1]) << 8);
      pos += 2;
      return true;
    };
    auto read_u32 = [&](uint32_t* out) -> bool {
      if (pos + 4 > payload.size()) return false;
      *out = static_cast<uint32_t>(payload[pos]) |
             (static_cast<uint32_t>(payload[pos + 1]) << 8) |
             (static_cast<uint32_t>(payload[pos + 2]) << 16) |
             (static_cast<uint32_t>(payload[pos + 3]) << 24);
      pos += 4;
      return true;
    };
    auto read_str16 = [&](std::string* out) -> bool {
      uint16_t len = 0;
      if (!read_u16(&len) || pos + len > payload.size()) return false;
      out->assign(reinterpret_cast<const char*>(payload.data() + pos), len);
      pos += len;
      return true;
    };

    uint32_t count = 0;
    if (read_u32(&count) && count <= 256) {
      std::vector<GuestAppCatalogEntry> entries;
      entries.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        GuestAppCatalogEntry e;
        uint8_t inst = 0;
        uint32_t icon_len = 0;
        if (!read_str16(&e.desktop_id) || !read_str16(&e.name) || !read_str16(&e.comment) ||
            !read_str16(&e.exec) || !read_str16(&e.apt_package) || !read_u8(&inst) ||
            !read_u32(&icon_len) || pos + icon_len > payload.size()) {
          break;
        }
        e.installed = (inst != 0);
        if (icon_len > 0) {
          e.icon_png.assign(payload.begin() + pos, payload.begin() + pos + icon_len);
          pos += icon_len;
        }
        entries.push_back(std::move(e));
      }
      LOGI(kTag, "Received guest Linux app catalog (%zu entries) over bridge", entries.size());
      engine->NotifyAppCatalogUpdated(std::move(entries));
    }
    return 0;
  }

  CommittedFrame frame;
  frame.surface_id = hdr.surface_id;
  frame.parent_surface_id = hdr.parent_surface_id;
  frame.frame_number = engine->NextFrameNumber();
  frame.title = std::move(title);
  frame.app_id = std::move(app_id);
  frame.icon_png = std::move(icon_png);
  frame.x = hdr.x;
  frame.y = hdr.y;
  frame.width = hdr.width;
  frame.height = hdr.height;
  frame.stride = hdr.stride;
  frame.format = hdr.format;
  frame.is_popup = (hdr.is_popup != 0);
  frame.is_subsurface = (hdr.is_subsurface != 0);
  frame.is_unmapped = (hdr.is_unmapped != 0);
  frame.buffer_type =
      (hdr.buffer_type == static_cast<uint32_t>(BufferType::kDmaBuf)) ? BufferType::kDmaBuf
                                                                      : BufferType::kShm;

  if (frame.buffer_type == BufferType::kDmaBuf && !payload.empty()) {
    // Reconstruct a host-side memfd DMA-BUF so VkRenderer imports it via VK_KHR_external_memory_fd
    int mfd = memfd_create("vkwlf-bridged-dmabuf", MFD_CLOEXEC);
    if (mfd >= 0 && ftruncate(mfd, static_cast<off_t>(payload.size())) == 0) {
      void* mapped = mmap(nullptr, payload.size(), PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
      if (mapped != MAP_FAILED) {
        std::memcpy(mapped, payload.data(), payload.size());
        munmap(mapped, payload.size());
        frame.dmabuf_fd = mfd;
        frame.planes.push_back(DmaBufPlane{
            .fd = dup(mfd),
            .offset = 0,
            .stride = frame.stride,
            .modifier = 0,
        });
      } else {
        close(mfd);
        frame.buffer_type = BufferType::kShm;
        frame.shm_pixels = std::move(payload);
      }
    } else {
      if (mfd >= 0) close(mfd);
      frame.buffer_type = BufferType::kShm;
      frame.shm_pixels = std::move(payload);
    }
  } else {
    frame.shm_pixels = std::move(payload);
  }

  if (!frame.is_popup && !frame.is_subsurface && !frame.is_unmapped) {
    static std::mutex s_launched_mu;
    static std::unordered_map<uint32_t, std::string> s_surface_titles;
    static std::unordered_map<uint32_t, bool> s_surface_has_icon;
    bool should_notify = false;
    {
      std::lock_guard<std::mutex> lock(s_launched_mu);
      auto it = s_surface_titles.find(frame.surface_id);
      if (it == s_surface_titles.end() || it->second != frame.title ||
          (!frame.icon_png.empty() && !s_surface_has_icon[frame.surface_id])) {
        s_surface_titles[frame.surface_id] = frame.title;
        if (!frame.icon_png.empty()) {
          s_surface_has_icon[frame.surface_id] = true;
        }
        should_notify = true;
      }
    }
    if (should_notify) {
      engine->NotifyAppLaunched(frame.surface_id, frame.title, frame.app_id, frame.icon_png);
    }
  }

  LOGV(kTag,
       "Received bridged guest frame #%llu (surface=%u, title='%s', app_id='%s', %ux%u, popup=%d, unmapped=%d)",
       static_cast<unsigned long long>(frame.frame_number), frame.surface_id,
       frame.title.c_str(), frame.app_id.c_str(), frame.width, frame.height,
       frame.is_popup ? 1 : 0, frame.is_unmapped ? 1 : 0);

  engine->PushCommittedFrame(std::move(frame));
  return 0;
}

bool WaylandServerEngine::SendFrameOverBridgeSocket(int sock_fd, const CommittedFrame& frame) {
  if (sock_fd < 0) return false;

  const uint8_t* pixel_ptr = nullptr;
  size_t pixel_bytes = 0;
  void* mapped_dmabuf = MAP_FAILED;
  const uint32_t plane_offset = frame.planes.empty() ? 0u : frame.planes[0].offset;
  size_t mapped_len =
      static_cast<size_t>(plane_offset) + static_cast<size_t>(frame.stride) * frame.height;

  if (!frame.is_unmapped) {
    if (!frame.shm_pixels.empty()) {
      pixel_ptr = frame.shm_pixels.data();
      pixel_bytes = frame.shm_pixels.size();
    } else if (frame.dmabuf_fd >= 0 && mapped_len > 0) {
      mapped_dmabuf = mmap(nullptr, mapped_len, PROT_READ, MAP_SHARED, frame.dmabuf_fd, 0);
      if (mapped_dmabuf != MAP_FAILED) {
        pixel_ptr = static_cast<const uint8_t*>(mapped_dmabuf) + plane_offset;
        pixel_bytes = static_cast<size_t>(frame.stride) * frame.height;
      }
    }
  }

  BridgeFrameHeader hdr{};
  hdr.magic = kBridgeFrameMagic;
  hdr.surface_id = frame.surface_id;
  hdr.parent_surface_id = frame.parent_surface_id;
  hdr.frame_number = frame.frame_number;
  hdr.x = frame.x;
  hdr.y = frame.y;
  hdr.width = frame.width;
  hdr.height = frame.height;
  hdr.stride = frame.stride;
  hdr.format = frame.format;
  hdr.buffer_type = static_cast<uint32_t>(BufferType::kDmaBuf);
  hdr.is_popup = frame.is_popup ? 1 : 0;
  hdr.is_subsurface = frame.is_subsurface ? 1 : 0;
  hdr.is_unmapped = frame.is_unmapped ? 1 : 0;
  hdr.title_len = static_cast<uint16_t>(std::min<size_t>(frame.title.size(), 4096));
  hdr.app_id_len = static_cast<uint16_t>(std::min<size_t>(frame.app_id.size(), 4096));
  hdr.icon_png_len = static_cast<uint32_t>(frame.icon_png.size());
  hdr.payload_bytes = static_cast<uint32_t>(pixel_bytes);

  bool ok = WriteAll(sock_fd, &hdr, sizeof(hdr));
  if (ok && hdr.title_len > 0) {
    ok = WriteAll(sock_fd, frame.title.data(), hdr.title_len);
  }
  if (ok && hdr.app_id_len > 0) {
    ok = WriteAll(sock_fd, frame.app_id.data(), hdr.app_id_len);
  }
  if (ok && hdr.icon_png_len > 0) {
    ok = WriteAll(sock_fd, frame.icon_png.data(), hdr.icon_png_len);
  }
  if (ok && pixel_bytes > 0 && pixel_ptr != nullptr) {
    ok = WriteAll(sock_fd, pixel_ptr, pixel_bytes);
  }

  if (mapped_dmabuf != MAP_FAILED) {
    munmap(mapped_dmabuf, mapped_len);
  }
  return ok;
}

void WaylandServerEngine::ForwardInputCommandToBridgeClients(const InputCommand& cmd) {
  if (cmd.type == InputCommand::Type::kQuit) return;

  BridgeInputPacket pkt{};
  pkt.magic = kBridgeInputMagic;
  pkt.type = static_cast<uint32_t>(cmd.type);
  pkt.time_ms = cmd.time_ms;
  pkt.x = cmd.x;
  pkt.y = cmd.y;
  pkt.button_or_key = cmd.button_or_key;
  pkt.state = cmd.state;
  pkt.mods_depressed = cmd.mods_depressed;
  pkt.mods_latched = cmd.mods_latched;
  pkt.mods_locked = cmd.mods_locked;
  pkt.group = cmd.group;
  if (cmd.type == InputCommand::Type::kLaunchApp) {
    pkt.button_or_key = static_cast<uint32_t>(std::min<size_t>(cmd.launch_spec.size(), 4096));
  } else if (cmd.type != InputCommand::Type::kKeyboardModifiers) {
    pkt.mods_locked = cmd.target_surface_id;
    pkt.mods_latched = cmd.host_width > 0 ? cmd.host_width : config_.display_width;
    pkt.group = cmd.host_height > 0 ? cmd.host_height : config_.display_height;
  }

  std::lock_guard<std::mutex> lock(bridge_clients_mutex_);
  for (auto& bc : bridge_clients_) {
    if (bc && bc->fd >= 0) {
      WriteAll(bc->fd, &pkt, sizeof(pkt));
      if (cmd.type == InputCommand::Type::kLaunchApp && pkt.button_or_key > 0) {
        WriteAll(bc->fd, cmd.launch_spec.data(), pkt.button_or_key);
      }
    }
  }
}

bool WaylandServerEngine::ProcessBridgeInputFromHost(int sock_fd) {
  BridgeInputPacket pkt{};
  if (!ReadAll(sock_fd, &pkt, sizeof(pkt)) || pkt.magic != kBridgeInputMagic) {
    return false;
  }
  auto type = static_cast<InputCommand::Type>(pkt.type);
  if (type == InputCommand::Type::kLaunchApp) {
    uint32_t len = pkt.button_or_key;
    std::string spec;
    if (len > 0 && len <= 4096) {
      spec.resize(len);
      if (!ReadAll(sock_fd, spec.data(), len)) {
        return false;
      }
    }
    if (spec == "__REFRESH_CATALOG__") {
      auto entries = DiscoverLocalGuestAppCatalog();
      SendAppCatalogOverBridgeSocket(sock_fd, entries);
      return true;
    }
    if (!spec.empty()) {
      LOGI(kTag, "Received host app launch request over bridge: '%s'", spec.c_str());
      const std::string sock_path = bound_socket_path_;
      std::thread([spec, sock_fd, sock_path]() {
        bool is_install = (spec.rfind("install:", 0) == 0);
        std::string target = is_install ? spec.substr(8) : spec;
        // Write to /tmp/vkwlf-xdg-<uid>/launch.req so vkwlf-auto-bridge or vkwlf-launch-app handles it
        std::string cmd = "/usr/local/bin/vkwlf-launch-app ";
        if ( !sock_path.empty()) {
          cmd += "--socket '" + sock_path + "' ";
        }
        if (is_install) {
          cmd += "--install ";
        }
        // Safely single-quote target
        std::string safe_target;
        for (char c : target) {
          if (c == '\'') {
            safe_target += "'\\''";
          } else {
            safe_target.push_back(c);
          }
        }
        cmd += "'" + safe_target + "' >/tmp/vkwlf-launch.log 2>&1";
        int rc = std::system(cmd.c_str());
        (void)rc;
        if (is_install) {
          auto updated = DiscoverLocalGuestAppCatalog();
          SendAppCatalogOverBridgeSocket(sock_fd, updated);
        }
      }).detach();
    }
    return true;
  }
  uint32_t target_sid = 0;
  uint32_t pkt_hw = 0;
  uint32_t pkt_hh = 0;
  if (type != InputCommand::Type::kKeyboardModifiers) {
    target_sid = pkt.mods_locked;
    pkt_hw = pkt.mods_latched;
    pkt_hh = pkt.group;
    if (pkt_hw > 0 && pkt_hh > 0) {
      host_display_width_ = pkt_hw;
      host_display_height_ = pkt_hh;
    }
    SetActiveInputSurface(target_sid, pkt_hw, pkt_hh);
  }
  switch (type) {
    case InputCommand::Type::kPointerMotion: {
      double mx = pkt.x;
      double my = pkt.y;
      uint32_t hw = pkt_hw > 0 ? pkt_hw : host_display_width_.load();
      uint32_t hh = pkt_hh > 0 ? pkt_hh : host_display_height_.load();
      int32_t cx = last_crop_x_.load();
      int32_t cy = last_crop_y_.load();
      uint32_t cw = last_crop_w_.load();
      uint32_t ch = last_crop_h_.load();
      if (target_sid != 0) {
        GetSurfaceGeometry(target_sid, &cx, &cy, &cw, &ch);
      }
      uint32_t host_w = hw > 0 ? hw : config_.display_width;
      uint32_t host_h = hh > 0 ? hh : config_.display_height;
      uint32_t surf_w = cw > 0 ? cw : config_.display_width;
      uint32_t surf_h = ch > 0 ? ch : config_.display_height;
      if (host_w > 0 && surf_w > 0 && host_w != surf_w) {
        mx = (mx * static_cast<double>(surf_w)) / static_cast<double>(host_w);
      }
      if (host_h > 0 && surf_h > 0 && host_h != surf_h) {
        my = (my * static_cast<double>(surf_h)) / static_cast<double>(host_h);
      }
      mx += static_cast<double>(cx);
      my += static_cast<double>(cy);
      SendPointerMotion(pkt.time_ms, mx, my);
      break;
    }
    case InputCommand::Type::kPointerButton:
      SendPointerButton(pkt.time_ms, pkt.button_or_key, pkt.state);
      break;
    case InputCommand::Type::kPointerAxis:
      SendPointerAxis(pkt.time_ms, pkt.button_or_key, pkt.x);
      break;
    case InputCommand::Type::kKeyboardKey:
      SendKeyboardKey(pkt.time_ms, pkt.button_or_key, pkt.state);
      break;
    case InputCommand::Type::kKeyboardModifiers:
      SendKeyboardModifiers(pkt.mods_depressed, pkt.mods_latched, pkt.mods_locked, pkt.group);
      break;
    case InputCommand::Type::kFocusSurface:
      FocusSurfaceById(target_sid != 0 ? target_sid : pkt.button_or_key);
      break;
    case InputCommand::Type::kDisplayResize: {
      uint32_t rw = static_cast<uint32_t>(pkt.x);
      uint32_t rh = static_cast<uint32_t>(pkt.y);
      if (rw > 0 && rh > 0) {
        LOGI(kTag, "Received host display resize over bridge: %ux%u (surface=%u)", rw, rh,
             target_sid);
        host_display_width_ = rw;
        host_display_height_ = rh;
        SetDisplaySizeForSurface(target_sid, rw, rh);
      }
      break;
    }
    default:
      break;
  }
  return true;
}

bool WaylandServerEngine::Start() {
  if (running_.exchange(true)) {
    return true;
  }
  server_thread_ = std::thread(&WaylandServerEngine::EventLoopThreadMain, this);
  return true;
}

void WaylandServerEngine::Stop() {
  if (running_.exchange(false)) {
    EnqueueInputCommand(InputCommand{.type = InputCommand::Type::kQuit});
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
  }

  {
    std::lock_guard<std::mutex> lock(bridge_clients_mutex_);
    for (auto& bc : bridge_clients_) {
      if (bc->source) {
        wl_event_source_remove(bc->source);
        bc->source = nullptr;
      }
      if (bc->fd >= 0) {
        close(bc->fd);
        bc->fd = -1;
      }
    }
    bridge_clients_.clear();
  }

  if (vsock_source_) {
    wl_event_source_remove(vsock_source_);
    vsock_source_ = nullptr;
  }
  if (vsock_fd_ >= 0) {
    close(vsock_fd_);
    vsock_fd_ = -1;
  }
  if (tcp_source_) {
    wl_event_source_remove(tcp_source_);
    tcp_source_ = nullptr;
  }
  if (tcp_fd_ >= 0) {
    close(tcp_fd_);
    tcp_fd_ = -1;
  }
  if (wakeup_source_) {
    wl_event_source_remove(wakeup_source_);
    wakeup_source_ = nullptr;
  }
  if (wakeup_pipe_[0] >= 0) {
    close(wakeup_pipe_[0]);
    wakeup_pipe_[0] = -1;
  }
  if (wakeup_pipe_[1] >= 0) {
    close(wakeup_pipe_[1]);
    wakeup_pipe_[1] = -1;
  }
  if (display_) {
    wl_display_destroy(display_);
    display_ = nullptr;
    event_loop_ = nullptr;
  }
  if (!bound_socket_path_.empty()) {
    unlink(bound_socket_path_.c_str());
    unlink((bound_socket_path_ + ".lock").c_str());
    bound_socket_path_.clear();
  }

  std::lock_guard<std::mutex> lock(frame_queue_mutex_);
  while (!frame_queue_.empty()) {
    auto& f = frame_queue_.front();
    if (f.dmabuf_fd >= 0) close(f.dmabuf_fd);
    for (auto& p : f.planes) {
      if (p.fd >= 0) close(p.fd);
    }
    frame_queue_.pop_front();
  }
  frame_queue_cv_.notify_all();
}

void WaylandServerEngine::EventLoopThreadMain() {
  LOGI(kTag, "Wayland background event-loop thread started");
  while (running_.load(std::memory_order_acquire)) {
    wl_display_flush_clients(display_);
    int ret = wl_event_loop_dispatch(event_loop_, 16);
    if (ret < 0 && errno != EINTR) {
      LOGE(kTag, "wl_event_loop_dispatch error: %s", strerror(errno));
      break;
    }
    ProcessPendingInputCommands();
    wl_display_flush_clients(display_);
  }
  LOGI(kTag, "Wayland background event-loop thread exiting");
}

int WaylandServerEngine::OnWakeupFdEvent(int fd, uint32_t /*mask*/, void* data) {
  char buf[64];
  while (read(fd, buf, sizeof(buf)) > 0) {
  }
  auto* self = static_cast<WaylandServerEngine*>(data);
  self->ProcessPendingInputCommands();
  return 0;
}

void WaylandServerEngine::EnqueueInputCommand(const InputCommand& cmd) {
  bool should_wake = false;
  {
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (cmd.type == InputCommand::Type::kPointerMotion && !input_queue_.empty() &&
        input_queue_.back().type == InputCommand::Type::kPointerMotion) {
      input_queue_.back() = cmd;
      return;
    }
    should_wake = input_queue_.empty();
    input_queue_.push_back(cmd);
  }
  if (should_wake && wakeup_pipe_[1] >= 0) {
    const uint8_t byte = 1;
    ssize_t unused = write(wakeup_pipe_[1], &byte, 1);
    (void)unused;
  }
}

void WaylandServerEngine::ProcessPendingInputCommands() {
  std::deque<InputCommand> local_cmds;
  {
    std::lock_guard<std::mutex> lock(input_mutex_);
    local_cmds.swap(input_queue_);
  }

  for (const auto& cmd : local_cmds) {
    if (cmd.type == InputCommand::Type::kQuit) {
      running_.store(false, std::memory_order_release);
      return;
    }

    // Forward input/resize commands to any connected remote guest bridge proxies
    ForwardInputCommandToBridgeClients(cmd);

    if (cmd.target_surface_id != 0) {
      for (const auto& entry : toplevel_entries_) {
        if (!entry.surface_res) continue;
        auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(entry.surface_res));
        if (surf && surf->id == cmd.target_surface_id) {
          preferred_base_surface_ = entry.surface_res;
          bool keep_child_focus = false;
          if (focused_surface_ && focused_surface_ != entry.surface_res &&
              cmd.type != InputCommand::Type::kFocusSurface &&
              wl_resource_get_client(focused_surface_) ==
                  wl_resource_get_client(entry.surface_res)) {
            auto* f_surf = static_cast<SurfaceState*>(wl_resource_get_user_data(focused_surface_));
            if (f_surf && (f_surf->is_popup || f_surf->parent_surface_id == surf->id)) {
              keep_child_focus = true;
            }
          }
          if (!focused_surface_ || (focused_surface_ != entry.surface_res && !keep_child_focus)) {
            SetFocusedSurface(entry.surface_res);
          }
          break;
        }
      }
    }

    if (cmd.type == InputCommand::Type::kFocusSurface ||
        cmd.type == InputCommand::Type::kLaunchApp) {
      continue;
    }

    if (cmd.type == InputCommand::Type::kDisplayResize) {
      uint32_t target_w = cmd.x > 0 ? static_cast<uint32_t>(cmd.x) : config_.display_width;
      uint32_t target_h = cmd.y > 0 ? static_cast<uint32_t>(cmd.y) : config_.display_height;
      if (cmd.target_surface_id == 0) {
        continue;
      }
      for (const auto& entry : toplevel_entries_) {
        if (entry.xdg_toplevel_res && entry.xdg_surface_res) {
          if (entry.surface_res) {
            auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(entry.surface_res));
            if (surf && surf->id != cmd.target_surface_id) {
              continue;
            }
          }
          struct wl_array states;
          wl_array_init(&states);
          auto* act = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
          if (act) *act = XDG_TOPLEVEL_STATE_ACTIVATED;
          auto* max_st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
          if (max_st) *max_st = XDG_TOPLEVEL_STATE_MAXIMIZED;
          xdg_toplevel_send_configure(entry.xdg_toplevel_res, static_cast<int32_t>(target_w),
                                      static_cast<int32_t>(target_h), &states);
          wl_array_release(&states);
          uint32_t serial = NextSerial();
          xdg_surface_send_configure(entry.xdg_surface_res, serial);
          LOGI(kTag, "Reconfigured toplevel surface (target=%u) to %ux%u (serial=%u)",
               cmd.target_surface_id, target_w, target_h, serial);
        }
      }
      continue;
    }

    if (cmd.type == InputCommand::Type::kPointerMotion) {
      if ((pending_left_press_ || scroll_drag_active_) && last_motion_time_ms_ != 0 &&
          cmd.time_ms > last_motion_time_ms_ && (cmd.time_ms - last_motion_time_ms_) > 350) {
        // Auto-recover if an Android gesture ended with ACTION_CANCEL without sending ACTION_UP
        pending_left_press_ = false;
        scroll_drag_active_ = false;
        pressed_buttons_mask_ = 0;
      }
      last_motion_time_ms_ = cmd.time_ms;
      if (!pending_left_press_ && !scroll_drag_active_) {
        double local_x = cmd.x;
        double local_y = cmd.y;
        struct wl_resource* target_surf = ResolvePointerTarget(cmd.x, cmd.y, &local_x, &local_y);
        last_pointer_x_ = local_x;
        last_pointer_y_ = local_y;
        if (target_surf && target_surf != focused_surface_) {
          SetFocusedSurface(target_surf);
        }
      }
    }

    if (focused_surface_ && !pointer_entered_ && !pointer_resources_.empty()) {
      uint32_t serial = NextSerial();
      for (auto* ptr : pointer_resources_) {
        if (wl_resource_get_client(ptr) != wl_resource_get_client(focused_surface_)) continue;
        wl_pointer_send_enter(ptr, serial, focused_surface_, wl_fixed_from_double(last_pointer_x_),
                              wl_fixed_from_double(last_pointer_y_));
        if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
          wl_pointer_send_frame(ptr);
        }
      }
      pointer_entered_ = true;
    }

    if (focused_surface_ && !keyboard_entered_ && !keyboard_resources_.empty()) {
      uint32_t serial = NextSerial();
      struct wl_array keys;
      wl_array_init(&keys);
      for (auto* kbd : keyboard_resources_) {
        if (wl_resource_get_client(kbd) != wl_resource_get_client(focused_surface_)) continue;
        wl_keyboard_send_enter(kbd, serial, focused_surface_, &keys);
      }
      wl_array_release(&keys);
      keyboard_entered_ = true;
    }

    switch (cmd.type) {
      case InputCommand::Type::kPointerMotion: {
        double local_x = cmd.x;
        double local_y = cmd.y;
        ResolvePointerTarget(cmd.x, cmd.y, &local_x, &local_y);
        if (pending_left_press_) {
          double total_dx = local_x - down_x_;
          double total_dy = local_y - down_y_;
          if (std::hypot(total_dx, total_dy) <= 8.0) {
            break;
          }
          pending_left_press_ = false;
          scroll_drag_active_ = true;
          prev_scroll_x_ = down_x_;
          prev_scroll_y_ = down_y_;
        }
        if (scroll_drag_active_) {
          double step_dx = local_x - prev_scroll_x_;
          double step_dy = local_y - prev_scroll_y_;
          prev_scroll_x_ = local_x;
          prev_scroll_y_ = local_y;
          for (auto* ptr : pointer_resources_) {
            if (focused_surface_ &&
                wl_resource_get_client(ptr) != wl_resource_get_client(focused_surface_)) {
              continue;
            }
            bool sent_axis = false;
            if (std::abs(step_dy) >= 0.5) {
              wl_pointer_send_axis(ptr, cmd.time_ms, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                   wl_fixed_from_double(-step_dy));
              sent_axis = true;
            }
            if (std::abs(step_dx) >= 0.5) {
              wl_pointer_send_axis(ptr, cmd.time_ms, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                   wl_fixed_from_double(-step_dx));
              sent_axis = true;
            }
            if (sent_axis && wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
              wl_pointer_send_frame(ptr);
            }
          }
          break;
        }
        wl_fixed_t fx = wl_fixed_from_double(local_x);
        wl_fixed_t fy = wl_fixed_from_double(local_y);
        for (auto* ptr : pointer_resources_) {
          if (focused_surface_ &&
              wl_resource_get_client(ptr) != wl_resource_get_client(focused_surface_)) {
            continue;
          }
          wl_pointer_send_motion(ptr, cmd.time_ms, fx, fy);
          if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
            wl_pointer_send_frame(ptr);
          }
        }
        break;
      }
      case InputCommand::Type::kPointerButton: {
        uint32_t btn_bit = 1u << (cmd.button_or_key & 0x1fu);
        if (cmd.state == WL_POINTER_BUTTON_STATE_PRESSED) {
          if ((pressed_buttons_mask_ & btn_bit) &&
              cmd.time_ms >= last_button_event_time_ms_ &&
              (cmd.time_ms - last_button_event_time_ms_) < 50) {
            break;
          }
          pressed_buttons_mask_ |= btn_bit;
          last_button_event_time_ms_ = cmd.time_ms;
          last_motion_time_ms_ = cmd.time_ms;
          if (cmd.button_or_key == 0x110) {
            pending_left_press_ = true;
            scroll_drag_active_ = false;
            down_time_ms_ = cmd.time_ms;
            down_x_ = last_pointer_x_;
            down_y_ = last_pointer_y_;
            prev_scroll_x_ = last_pointer_x_;
            prev_scroll_y_ = last_pointer_y_;
            break;
          }
        } else {
          if (!(pressed_buttons_mask_ & btn_bit)) {
            break;
          }
          pressed_buttons_mask_ &= ~btn_bit;
          last_button_event_time_ms_ = cmd.time_ms;
          if (cmd.button_or_key == 0x110) {
            if (pending_left_press_) {
              pending_left_press_ = false;
              wl_fixed_t fx = wl_fixed_from_double(down_x_);
              wl_fixed_t fy = wl_fixed_from_double(down_y_);
              uint32_t serial_down = NextSerial();
              uint32_t serial_up = NextSerial();
              for (auto* ptr : pointer_resources_) {
                if (focused_surface_ &&
                    wl_resource_get_client(ptr) != wl_resource_get_client(focused_surface_)) {
                  continue;
                }
                wl_pointer_send_motion(ptr, down_time_ms_, fx, fy);
                wl_pointer_send_button(ptr, serial_down, down_time_ms_, cmd.button_or_key,
                                       WL_POINTER_BUTTON_STATE_PRESSED);
                if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
                  wl_pointer_send_frame(ptr);
                }
                wl_pointer_send_button(ptr, serial_up, cmd.time_ms, cmd.button_or_key,
                                       WL_POINTER_BUTTON_STATE_RELEASED);
                if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
                  wl_pointer_send_frame(ptr);
                }
              }
              break;
            }
            if (scroll_drag_active_) {
              scroll_drag_active_ = false;
              break;
            }
          }
        }
        uint32_t serial = NextSerial();
        wl_fixed_t fx = wl_fixed_from_double(last_pointer_x_);
        wl_fixed_t fy = wl_fixed_from_double(last_pointer_y_);
        for (auto* ptr : pointer_resources_) {
          if (focused_surface_ &&
              wl_resource_get_client(ptr) != wl_resource_get_client(focused_surface_)) {
            continue;
          }
          wl_pointer_send_motion(ptr, cmd.time_ms, fx, fy);
          wl_pointer_send_button(ptr, serial, cmd.time_ms, cmd.button_or_key, cmd.state);
          if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
            wl_pointer_send_frame(ptr);
          }
        }
        break;
      }
      case InputCommand::Type::kPointerAxis: {
        wl_fixed_t fval = wl_fixed_from_double(cmd.x);
        for (auto* ptr : pointer_resources_) {
          if (focused_surface_ &&
              wl_resource_get_client(ptr) != wl_resource_get_client(focused_surface_)) {
            continue;
          }
          wl_pointer_send_axis(ptr, cmd.time_ms, cmd.button_or_key, fval);
          if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
            wl_pointer_send_frame(ptr);
          }
        }
        break;
      }
      case InputCommand::Type::kKeyboardKey: {
        uint32_t serial = NextSerial();
        for (auto* kbd : keyboard_resources_) {
          if (focused_surface_ &&
              wl_resource_get_client(kbd) != wl_resource_get_client(focused_surface_)) {
            continue;
          }
          wl_keyboard_send_key(kbd, serial, cmd.time_ms, cmd.button_or_key, cmd.state);
        }
        break;
      }
      case InputCommand::Type::kKeyboardModifiers: {
        uint32_t serial = NextSerial();
        for (auto* kbd : keyboard_resources_) {
          if (focused_surface_ &&
              wl_resource_get_client(kbd) != wl_resource_get_client(focused_surface_)) {
            continue;
          }
          wl_keyboard_send_modifiers(kbd, serial, cmd.mods_depressed, cmd.mods_latched,
                                     cmd.mods_locked, cmd.group);
        }
        break;
      }
      case InputCommand::Type::kDisplayResize:
      case InputCommand::Type::kQuit:
        break;
    }
  }
}

void WaylandServerEngine::PushCommittedFrame(CommittedFrame&& frame) {
  {
    std::lock_guard<std::mutex> lock(frame_queue_mutex_);
    if (!frame.is_unmapped) {
      for (auto& existing : frame_queue_) {
        if (existing.surface_id == frame.surface_id && !existing.is_unmapped) {
          if (frame.icon_png.empty() && !existing.icon_png.empty()) {
            frame.icon_png = std::move(existing.icon_png);
          }
          if (existing.dmabuf_fd >= 0) close(existing.dmabuf_fd);
          for (auto& p : existing.planes) {
            if (p.fd >= 0) close(p.fd);
          }
          existing = std::move(frame);
          frame_queue_cv_.notify_one();
          return;
        }
      }
    }
    if (frame_queue_.size() >= 16) {
      auto& dropped = frame_queue_.front();
      if (dropped.dmabuf_fd >= 0) close(dropped.dmabuf_fd);
      for (auto& p : dropped.planes) {
        if (p.fd >= 0) close(p.fd);
      }
      frame_queue_.pop_front();
    }
    frame_queue_.push_back(std::move(frame));
  }
  frame_queue_cv_.notify_one();
}

bool WaylandServerEngine::PopCommittedFrame(CommittedFrame* out_frame, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(frame_queue_mutex_);
  if (frame_queue_.empty() && timeout_ms > 0) {
    frame_queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this]() { return !frame_queue_.empty() || !running_.load(); });
  }
  if (frame_queue_.empty()) {
    return false;
  }
  *out_frame = std::move(frame_queue_.front());
  frame_queue_.pop_front();
  return true;
}

size_t WaylandServerEngine::PendingFrameCount() const {
  std::lock_guard<std::mutex> lock(frame_queue_mutex_);
  return frame_queue_.size();
}

void WaylandServerEngine::SendPointerMotion(uint32_t time_ms, double x, double y) {
  EnqueueInputCommand(InputCommand{
      .type = InputCommand::Type::kPointerMotion,
      .time_ms = time_ms,
      .x = x,
      .y = y,
      .target_surface_id = active_input_surface_id_.load(),
      .host_width = active_input_width_.load(),
      .host_height = active_input_height_.load(),
  });
}

void WaylandServerEngine::SendPointerButton(uint32_t time_ms, uint32_t button, uint32_t state) {
  EnqueueInputCommand(InputCommand{
      .type = InputCommand::Type::kPointerButton,
      .time_ms = time_ms,
      .button_or_key = button,
      .state = state,
      .target_surface_id = active_input_surface_id_.load(),
      .host_width = active_input_width_.load(),
      .host_height = active_input_height_.load(),
  });
}

void WaylandServerEngine::SendPointerAxis(uint32_t time_ms, uint32_t axis, double value) {
  EnqueueInputCommand(InputCommand{
      .type = InputCommand::Type::kPointerAxis,
      .time_ms = time_ms,
      .x = value,
      .button_or_key = axis,
      .target_surface_id = active_input_surface_id_.load(),
      .host_width = active_input_width_.load(),
      .host_height = active_input_height_.load(),
  });
}

void WaylandServerEngine::SendKeyboardKey(uint32_t time_ms, uint32_t key, uint32_t state) {
  EnqueueInputCommand(InputCommand{
      .type = InputCommand::Type::kKeyboardKey,
      .time_ms = time_ms,
      .button_or_key = key,
      .state = state,
      .target_surface_id = active_input_surface_id_.load(),
      .host_width = active_input_width_.load(),
      .host_height = active_input_height_.load(),
  });
}

void WaylandServerEngine::SendKeyboardModifiers(uint32_t mods_depressed,
                                                uint32_t mods_latched,
                                                uint32_t mods_locked,
                                                uint32_t group) {
  EnqueueInputCommand(InputCommand{
      .type = InputCommand::Type::kKeyboardModifiers,
      .mods_depressed = mods_depressed,
      .mods_latched = mods_latched,
      .mods_locked = mods_locked,
      .group = group,
      .target_surface_id = active_input_surface_id_.load(),
  });
}

void WaylandServerEngine::SetDisplaySize(uint32_t width, uint32_t height) {
  SetDisplaySizeForSurface(0, width, height);
}

void WaylandServerEngine::SetDisplaySizeForSurface(uint32_t surface_id,
                                                   uint32_t width,
                                                   uint32_t height) {
  if (width == 0 || height == 0) return;
  config_.display_width = width;
  config_.display_height = height;
  EnqueueInputCommand(InputCommand{
      .type = InputCommand::Type::kDisplayResize,
      .x = static_cast<double>(width),
      .y = static_cast<double>(height),
      .target_surface_id = surface_id,
      .host_width = width,
      .host_height = height,
  });
}

void WaylandServerEngine::SetActiveInputSurface(uint32_t surface_id,
                                                uint32_t host_width,
                                                uint32_t host_height) {
  active_input_surface_id_.store(surface_id);
  if (host_width > 0) active_input_width_.store(host_width);
  if (host_height > 0) active_input_height_.store(host_height);
}

void WaylandServerEngine::FocusSurfaceById(uint32_t surface_id) {
  if (surface_id == 0) return;
  active_input_surface_id_.store(surface_id);
  EnqueueInputCommand(InputCommand{
      .type = InputCommand::Type::kFocusSurface,
      .button_or_key = surface_id,
      .target_surface_id = surface_id,
      .host_width = active_input_width_.load(),
      .host_height = active_input_height_.load(),
  });
}

void WaylandServerEngine::SetToplevelGeometry(int32_t crop_x,
                                              int32_t crop_y,
                                              uint32_t crop_w,
                                              uint32_t crop_h) {
  last_crop_x_ = crop_x;
  last_crop_y_ = crop_y;
  if (crop_w > 0) last_crop_w_ = crop_w;
  if (crop_h > 0) last_crop_h_ = crop_h;
}

void WaylandServerEngine::RecordSurfaceGeometry(uint32_t surface_id,
                                                int32_t crop_x,
                                                int32_t crop_y,
                                                uint32_t crop_w,
                                                uint32_t crop_h) {
  if (surface_id == 0 || crop_w == 0 || crop_h == 0) return;
  std::lock_guard<std::mutex> lock(surface_geom_mutex_);
  surface_geometries_[surface_id] = SurfaceGeomInfo{
      .crop_x = crop_x,
      .crop_y = crop_y,
      .crop_w = crop_w,
      .crop_h = crop_h,
  };
}

bool WaylandServerEngine::GetSurfaceGeometry(uint32_t surface_id,
                                             int32_t* out_crop_x,
                                             int32_t* out_crop_y,
                                             uint32_t* out_crop_w,
                                             uint32_t* out_crop_h) const {
  std::lock_guard<std::mutex> lock(surface_geom_mutex_);
  auto it = surface_geometries_.find(surface_id);
  if (it == surface_geometries_.end()) return false;
  if (out_crop_x) *out_crop_x = it->second.crop_x;
  if (out_crop_y) *out_crop_y = it->second.crop_y;
  if (out_crop_w) *out_crop_w = it->second.crop_w;
  if (out_crop_h) *out_crop_h = it->second.crop_h;
  return true;
}

uint32_t WaylandServerEngine::NextSerial() {
  return display_ ? wl_display_next_serial(display_) : 1;
}

void WaylandServerEngine::RegisterClientPointer(struct wl_resource* pointer_res) {
  pointer_resources_.push_back(pointer_res);
  if (focused_surface_ &&
      wl_resource_get_client(pointer_res) == wl_resource_get_client(focused_surface_)) {
    uint32_t serial = NextSerial();
    wl_pointer_send_enter(pointer_res, serial, focused_surface_,
                          wl_fixed_from_double(last_pointer_x_),
                          wl_fixed_from_double(last_pointer_y_));
    if (wl_resource_get_version(pointer_res) >= WL_POINTER_FRAME_SINCE_VERSION) {
      wl_pointer_send_frame(pointer_res);
    }
    pointer_entered_ = true;
  }
}

void WaylandServerEngine::UnregisterClientPointer(struct wl_resource* pointer_res) {
  pointer_resources_.erase(
      std::remove(pointer_resources_.begin(), pointer_resources_.end(), pointer_res),
      pointer_resources_.end());
}

int WaylandServerEngine::CreateKeymapFd(size_t* out_size) {
  std::string keymap_bytes;
  for (const char* path : {"/usr/local/share/vkwlf/default.xkb",
                           "/home/droid/VlkWlfApp/guest/default.xkb"}) {
    std::ifstream in(path, std::ios::binary);
    if (in.is_open()) {
      std::ostringstream ss;
      ss << in.rdbuf();
      keymap_bytes = ss.str();
      if (!keymap_bytes.empty()) {
        keymap_bytes.push_back('\0');
        break;
      }
    }
  }
  const char* src_data = keymap_bytes.empty() ? kMinimalXkbKeymap : keymap_bytes.data();
  const size_t len = keymap_bytes.empty() ? sizeof(kMinimalXkbKeymap) : keymap_bytes.size();
  int fd = memfd_create("vkwlf-xkb-keymap", MFD_CLOEXEC);
  if (fd < 0) return -1;
  if (ftruncate(fd, static_cast<off_t>(len)) < 0) {
    close(fd);
    return -1;
  }
  void* ptr = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    close(fd);
    return -1;
  }
  std::memcpy(ptr, src_data, len);
  munmap(ptr, len);
  *out_size = len;
  return fd;
}

void WaylandServerEngine::RegisterClientKeyboard(struct wl_resource* keyboard_res) {
  keyboard_resources_.push_back(keyboard_res);

  size_t keymap_size = 0;
  int keymap_fd = CreateKeymapFd(&keymap_size);
  if (keymap_fd >= 0) {
    wl_keyboard_send_keymap(keyboard_res, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keymap_fd,
                            static_cast<uint32_t>(keymap_size));
    close(keymap_fd);
  }
  if (wl_resource_get_version(keyboard_res) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
    wl_keyboard_send_repeat_info(keyboard_res, 30, 400);
  }

  if (focused_surface_ &&
      wl_resource_get_client(keyboard_res) == wl_resource_get_client(focused_surface_)) {
    uint32_t serial = NextSerial();
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(keyboard_res, serial, focused_surface_, &keys);
    wl_array_release(&keys);
    keyboard_entered_ = true;
  }
}

void WaylandServerEngine::UnregisterClientKeyboard(struct wl_resource* keyboard_res) {
  keyboard_resources_.erase(
      std::remove(keyboard_resources_.begin(), keyboard_resources_.end(), keyboard_res),
      keyboard_resources_.end());
}

void WaylandServerEngine::RegisterToplevelSurface(struct wl_resource* surface_res,
                                                  struct wl_resource* xdg_surface_res,
                                                  struct wl_resource* xdg_toplevel_res) {
  toplevel_entries_.push_back(ToplevelEntry{
      .surface_res = surface_res,
      .xdg_surface_res = xdg_surface_res,
      .xdg_toplevel_res = xdg_toplevel_res,
  });
}

void WaylandServerEngine::UnregisterToplevelSurface(struct wl_resource* xdg_toplevel_res) {
  toplevel_entries_.erase(
      std::remove_if(toplevel_entries_.begin(), toplevel_entries_.end(),
                     [xdg_toplevel_res](const ToplevelEntry& e) {
                       return e.xdg_toplevel_res == xdg_toplevel_res;
                     }),
      toplevel_entries_.end());
}

void WaylandServerEngine::OnSurfaceUnmappedOrDestroyed(struct wl_resource* surface_res,
                                                       uint32_t surface_id,
                                                       bool was_drawable) {
  if (preferred_base_surface_ == surface_res) {
    preferred_base_surface_ = nullptr;
  }
  if (surface_id != 0) {
    std::lock_guard<std::mutex> lock(surface_geom_mutex_);
    surface_geometries_.erase(surface_id);
  }
  if (focused_surface_ == surface_res) {
    struct wl_resource* fallback = nullptr;
    if (!toplevel_entries_.empty()) {
      fallback = toplevel_entries_.back().surface_res;
    }
    SetFocusedSurface(fallback);
  }
  if (was_drawable && surface_id != 0) {
    CommittedFrame unmap_frame;
    unmap_frame.surface_id = surface_id;
    unmap_frame.frame_number = NextFrameNumber();
    unmap_frame.is_unmapped = true;
    LOGD(kTag, "Surface %u unmapped/destroyed; notifying renderer", surface_id);
    PushCommittedFrame(std::move(unmap_frame));
  }
}

struct wl_resource* WaylandServerEngine::ResolvePointerTarget(double screen_x,
                                                              double screen_y,
                                                              double* out_local_x,
                                                              double* out_local_y) const {
  if (out_local_x) *out_local_x = screen_x;
  if (out_local_y) *out_local_y = screen_y;

  size_t mapped_count = 0;
  struct wl_resource* base_surface = nullptr;
  for (const auto& entry : toplevel_entries_) {
    if (!entry.surface_res) continue;
    auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(entry.surface_res));
    if (!surf || !surf->has_committed_buffer || surf->committed_width == 0 ||
        surf->committed_height == 0) {
      continue;
    }
    mapped_count++;
    if (entry.surface_res == preferred_base_surface_) {
      base_surface = entry.surface_res;
    } else if (base_surface != preferred_base_surface_ &&
               (!base_surface ||
                (surf->committed_width >= (config_.display_width * 3) / 4 &&
                 surf->committed_height >= (config_.display_height * 3) / 4))) {
      base_surface = entry.surface_res;
    }
  }

  if (mapped_count > 1) {
    auto* base_surf = base_surface
                          ? static_cast<SurfaceState*>(wl_resource_get_user_data(base_surface))
                          : nullptr;
    for (auto it = toplevel_entries_.rbegin(); it != toplevel_entries_.rend(); ++it) {
      if (!it->surface_res || it->surface_res == base_surface) continue;
      if (base_surface &&
          wl_resource_get_client(it->surface_res) != wl_resource_get_client(base_surface)) {
        continue;
      }
      auto* surf = static_cast<SurfaceState*>(wl_resource_get_user_data(it->surface_res));
      if (!surf || !surf->has_committed_buffer || surf->committed_width == 0 ||
          surf->committed_height == 0) {
        continue;
      }
      const bool is_dialog_size =
          (base_surf && surf->parent_surface_id == base_surf->id) ||
          (surf->parent_surface_id == 0 &&
           (surf->committed_width < 640 || surf->committed_height < 400));
      if (!is_dialog_size) continue;

      int32_t dx = (static_cast<int32_t>(config_.display_width) -
                    static_cast<int32_t>(surf->committed_width)) /
                   2;
      int32_t dy = (static_cast<int32_t>(config_.display_height) -
                    static_cast<int32_t>(surf->committed_height)) /
                   2;
      if (dx < 0) dx = 0;
      if (dy < 0) dy = 0;

      if (screen_x >= dx && screen_x < dx + static_cast<double>(surf->committed_width) &&
          screen_y >= dy && screen_y < dy + static_cast<double>(surf->committed_height)) {
        if (out_local_x) *out_local_x = screen_x - dx;
        if (out_local_y) *out_local_y = screen_y - dy;
        return it->surface_res;
      }
    }
  }

  return base_surface ? base_surface : focused_surface_;
}

void WaylandServerEngine::SetFocusedSurface(struct wl_resource* surface_res) {
  if (focused_surface_ == surface_res) return;

  if (focused_surface_) {
    if (pointer_entered_) {
      uint32_t serial = NextSerial();
      for (auto* ptr : pointer_resources_) {
        if (wl_resource_get_client(ptr) != wl_resource_get_client(focused_surface_)) continue;
        wl_pointer_send_leave(ptr, serial, focused_surface_);
        if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
          wl_pointer_send_frame(ptr);
        }
      }
    }
    if (keyboard_entered_) {
      uint32_t serial = NextSerial();
      for (auto* kbd : keyboard_resources_) {
        if (wl_resource_get_client(kbd) != wl_resource_get_client(focused_surface_)) continue;
        wl_keyboard_send_leave(kbd, serial, focused_surface_);
      }
    }
  }

  focused_surface_ = surface_res;
  pointer_entered_ = false;
  keyboard_entered_ = false;

  if (focused_surface_) {
    uint32_t serial = NextSerial();
    for (auto* ptr : pointer_resources_) {
      if (wl_resource_get_client(ptr) != wl_resource_get_client(focused_surface_)) continue;
      wl_pointer_send_enter(ptr, serial, focused_surface_,
                            wl_fixed_from_double(last_pointer_x_),
                            wl_fixed_from_double(last_pointer_y_));
      if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(ptr);
      }
      pointer_entered_ = true;
    }

    struct wl_array keys;
    wl_array_init(&keys);
    for (auto* kbd : keyboard_resources_) {
      if (wl_resource_get_client(kbd) != wl_resource_get_client(focused_surface_)) continue;
      wl_keyboard_send_enter(kbd, NextSerial(), focused_surface_, &keys);
      keyboard_entered_ = true;
    }
    wl_array_release(&keys);
  }
}

}  // namespace vkwlf
