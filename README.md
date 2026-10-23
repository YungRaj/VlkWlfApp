# VkWlfApp — Linux Wayland apps as first-class windows on Android desktop (experimental)

**Status: experimental / prototype. Not for production.**

VkWlfApp is an Android app that embeds a Wayland compositor and presents Linux
desktop apps (Antigravity, an Electron IDE, is the reference workload) as native Android windows:

- Wayland server (`wl_compositor`, `xdg-shell`, `linux-dmabuf`) in
  [`src/wayland_server_engine.cpp`](src/wayland_server_engine.cpp)
- Zero‑copy presentation: client dma‑bufs are imported as `AHardwareBuffer`s
  (`VK_ANDROID_external_memory_android_hardware_buffer`) in
  [`src/vk_renderer.cpp`](src/vk_renderer.cpp) — no shm uploads
- One Android window per toplevel, with the Linux app's own icon and title
  (dynamic `FabricatedOverlay`), input routing, focus
- A vsock/TCP "bridge" transport and an in‑guest proxy
  ([`src/vkwlf_guest_proxy.cpp`](src/vkwlf_guest_proxy.cpp)) for clients that
  run inside a VM

## Screenshots (Antigravity IDE on VkWlfApp)

Full **Antigravity IDE** workbench (`2382×1299`) running inside the Linux VM over the `vkwlf_guest_proxy` Wayland bridge, with the project explorer, C++ editor (`clangd`), integrated terminal, and AI Agent panel active:

![Antigravity IDE running in VkWlfApp](docs/images/antigravity_ide_vkwlf.png)

<details>
<summary><b>Close-up: Right-side AI Agent Panel (Claude 5.5 / Gemini)</b></summary>
<br>

![Antigravity IDE Agent Panel](docs/images/antigravity_agent_panel.png)

</details>

## Layout

| Path | What |
|---|---|
| `src/`, `include/` | compositor, Vulkan renderer, input, guest proxy |
| `java/`, `res/`, `assets/`, `AndroidManifest.xml` | the Android app (`com.google.example.vkwlf`) |
| `Android.bp`, `CMakeLists.txt` | Soong and CMake builds |
| `tools/build_apk.sh` | reproducible APK build (NDK + aapt2/d8/apksigner) |
| `guest/vkwlf-run` | in‑VM launcher: starts the proxy and runs a Wayland app against it |
| `guest/vkwlf-auto-bridge` | persistent auto-bridge service script for launching Antigravity IDE |
| `guest/vkwlf-cloudcode-proxy` | local Cloud Code model compatibility proxy for Antigravity IDE |
| `guest/` (rest) | dev‑rig only: host daemon, IDE chroot launcher, `libnopkey.c` shim |
| `tools/` (rest) | dev‑rig only: VM viewer launcher, VNC colour‑fix proxy, QEMU wrapper |
| `tests/` | `weston-simple-shm`, a minimal Wayland client used as the smoke test |

---

# Running on a real Android desktop device (release build)

On a release build the Linux app runs inside the stock **Terminal** VM and
VkWlfApp runs as a normal Android app. The two talk over TCP on the VM's virtual
network:

```
Terminal VM (Debian, KVM)                         Android (host)
  <wayland app> ──wayland──▶ vkwlf_guest_proxy ──TCP──▶ VkWlfApp ──AHardwareBuffer──▶ SurfaceFlinger
                             (guest/vkwlf-run)         bridge :5555
```

No root, no unlock, no flashing. Raster happens on the CPU inside the VM (the
Terminal VM's virtio‑gpu is 2D‑only); composition/presentation uses the host GPU.

## 1. Build and install VkWlfApp (on your workstation)

Requirements: Android SDK (`aapt2`, `d8`, `apksigner`, `zipalign`, `android.jar`),
an NDK, `cmake` ≥ 3.18, `wayland-scanner`, a JDK, and libwayland + libffi
*sources* (the NDK build compiles libwayland‑server from source). Point the variables below at your SDK, NDK and source checkouts.

```bash
# required
export ANDROID_SDK_ROOT=~/Android/Sdk
export ANDROID_NDK_ROOT=~/Android/Sdk/ndk/27.2.12479018
export VKWLF_WAYLAND_SRC=~/src/wayland          # git clone https://gitlab.freedesktop.org/wayland/wayland
export VKWLF_LIBFFI_SRC=~/src/libffi            # needs generated ffi.h for the target ABI

tools/build_apk.sh --abi x86_64                 # Intel Android desktop device; use arm64-v8a on Arm
adb install -r out/apk/VkWlfApp.apk             # or: tools/build_apk.sh --install <serial>
```

Start **VkWlfApp** once from the launcher. In `adb logcat` you should see:

```
VkWlfCompositor: Wayland display initialized; Unix socket bound at /data/user/<u>/com.google.example.vkwlf/cache/wayland-0
VkWlfCompositor: Bound TCP Wayland Bridge listener on 0.0.0.0:5555
```

> If the line says `0.0.0.0:5556`, port 5555 was busy (adbd takes it when
> wireless debugging is on). Use that port in step 3. `AF_VSOCK not available
> (Permission denied)` is expected for a normal app — TCP is the transport.

## 2. Build the guest side inside the Terminal VM

Open the Terminal app, then in the VM:

```bash
sudo apt update
sudo apt install -y git build-essential cmake pkg-config libwayland-bin \
    libwayland-dev wayland-protocols libvulkan-dev mesa-vulkan-drivers weston

git clone https://github.com/YungRaj/VlkWlfApp.git && cd VlkWlfApp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target vkwlf_guest_proxy weston-simple-shm -j"$(nproc)"
sudo install -m755 build/vkwlf_guest_proxy guest/vkwlf-run /usr/local/bin/
```

(`wayland-scanner` is in `libwayland-bin` on Debian; `mesa-vulkan-drivers`
provides lavapipe so the proxy's Vulkan init has a device.)

## 3. Run a candidate Wayland app

`vkwlf-run` starts the proxy on a private `XDG_RUNTIME_DIR`, points it at the
host (default: the VM's default gateway, which is Android), and runs the app
with `WAYLAND_DISPLAY` set:

```bash
# smoke test — a 1600x900 test pattern should appear as its own Android window
vkwlf-run -- weston-simple-shm

# a real app
vkwlf-run -- weston-terminal
vkwlf-run -- foot                                   # apt install foot

# different bridge port (see note in step 1)
vkwlf-run --port 5556 -- weston-simple-shm
```

For **Antigravity** (a public Electron IDE) inside the VM —
install the Linux `.deb` from antigravity.google first, then:

```bash
vkwlf-run -- antigravity --disable-gpu ~/workspace
```

`vkwlf-run` already exports `ELECTRON_OZONE_PLATFORM_HINT=wayland`,
`GDK_BACKEND=wayland` and `QT_QPA_PLATFORM=wayland`, so Electron, GTK and Qt
apps pick the Wayland backend without extra flags. For an Electron app that
ignores the hint, add `--ozone-platform=wayland --enable-features=UseOzonePlatform`.
`--disable-gpu` is correct here: the Terminal VM has no 3D GPU, so Chromium
rasterises with SwiftShader; dropping the flag only adds a failed GPU probe.

The window shows up on the Android desktop with the app's icon and title
(resolved from its `app_id` via the guest's `/usr/share/applications` +
`hicolor` icon theme — `antigravity.desktop` / `antigravity.png` from the `.deb`).

How the caption gets the right name/icon on a **release (user) build**: the
desktop window header is drawn by SystemUI from the *host package's* static
label/icon, and a normal app cannot change that at runtime (a self-targeting
`FabricatedOverlay` is only visible to the app itself). VkWlfApp therefore
requests a transparent caption-bar background — the system then stops drawing
the app name and keeps only its small launcher badge + app-menu chevron on the
left and the window controls on the right — and draws the Linux app's real icon
and name itself in the gap (bounds from `WindowInsets.getBoundingRects(captionBar())`).
The `TaskDescription` is also set so Recents shows the Linux app. If you ever
installed a static overlay (`com.google.example.vkwlf.overlay`) during
development, uninstall it; SystemUI caches the overlaid label until it restarts.

What has been verified end to end over the bridge with an Electron IDE
(Antigravity build): continuous frames (~60 fps at 1280×800), window
title/app_id/icon, keyboard input landing in the editor, pointer clicks, and
`xdg_popup` menus (File menu). Any `wl_shm` + `xdg-shell` client (GTK, Qt,
SDL, Electron, weston clients) takes the same path; 3D/GL apps render via
llvmpipe inside the VM and are out of scope.

## 4. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `vkwlf-run: proxy exited: Failed to connect to host VkWlfCompositor` | VkWlfApp not running, wrong port, or the VM→host route is blocked. Check `ip route` in the VM for the gateway, `nc -vz <gateway> 5555`, and the logcat line from step 1. |
| Window appears but stays black | Client is using a buffer format the bridge doesn't forward; try `weston-simple-shm` first, then compare `logcat | grep VkWlfCompositor`. |
| Keys/mouse don't reach the app | Tap the window once to give it Android focus; check `logcat | grep InputHandler`. |
| `Bound TCP Wayland Bridge listener on 0.0.0.0:5556` | 5555 taken by adbd; pass `--port 5556`. |
| Slow scrolling / animation | Expected on this path (CPU raster + one copy per frame). GPU raster in the VM needs the Terminal VM's `gpu.backend` changed from `2d` (platform change) — see `docs/android_desktop_linux_apps_roadmap.md`. |

---

# Dev rig (how this snapshot was developed)

Development was done against a cuttlefish x86_64 desktop (Android desktop)
VM running under **QEMU TCG** (no KVM on the workstation), with the IDE in a
host‑side chroot (userdebug only). Several things in `guest/` and `tools/` exist
only to survive TCG and are **not needed on real hardware**:

- `libnopkey.so` (disables PKU, pins each Electron process to one vCPU)
- interpreter‑only V8 flags in `guest/run_linux_app.sh`
- `printk=0` in the daemon (virtio‑console traffic corrupts guest memory under MTTCG)
- the R/B‑swap VNC proxy (QEMU mislabels the virtio‑gpu scanout)

See `docs/` for the analysis and the roadmap:

- [`docs/tcg_crash_fix_walkthrough.md`](docs/tcg_crash_fix_walkthrough.md) — what broke under TCG and why
- [`docs/android_desktop_linux_apps_roadmap.md`](docs/android_desktop_linux_apps_roadmap.md) — phased plan to GPU raster / zero‑copy on release builds
- [`docs/wayland_direct_host_gpu_walkthrough.md`](docs/wayland_direct_host_gpu_walkthrough.md) — the direct‑on‑host GPU path (userdebug)
