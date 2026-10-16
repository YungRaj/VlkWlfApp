# Direct-on-Host Wayland App Launch & Host GPU Rendering on Android desktop (No pKVM)

## What Was Completed & Verified

1. **Direct-on-Host Wayland App Launch Without pKVM**:
   - The full `/opt/example-ide/example-ide` Linux Wayland application runs directly on the Android desktop Android host inside `/data/local/linux-root` (sharing the host kernel `Linux 6.12.92-android16 x86_64`), with `/data/user/10/com.google.example.vkwlf/cache` bind-mounted to `/data/local/linux-root/tmp` so it connects directly to the UNIX domain Wayland socket `wayland-0` created by [WaylandServerEngine::Init](../src/wayland_server_engine.cpp#L685-L760).
   - Clicking the app icon in the Android desktop dock invokes [MainActivity.ensureDirectHostWaylandClientRunning](../java/com/google/example/vkwlf/MainActivity.java#L115-L140), which triggers `/data/local/linux_host_daemon.sh` to launch `/opt/example-ide/example-ide --ozone-platform=wayland` automatically without pKVM (`crosvm` / Ferrochrome) or a Linux terminal.

2. **100% Host GPU `AHardwareBuffer` Rendering (`shm_uploads = 0`)**:
   - Every Wayland frame buffer is imported into an Android GPU `AHardwareBuffer` (`AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE`) and bound to a Vulkan `VkImage` via `VK_ANDROID_external_memory_android_hardware_buffer` in [VkRenderer::ImportDmaBufToTexture](../src/vk_renderer.cpp#L1265-L1440) and [VkRenderer::ImportAndroidHardwareBuffer](../src/vk_renderer.cpp#L1593-L1723), then composited and presented via `vkQueuePresentKHR` with `shm_uploads=0`.

3. **Font Rendering Fixed in Direct-on-Host Runtime**:
   - Installed full `/usr/share/fontconfig/conf.avail` alias rules and standard `Noto Sans`, `Noto Sans Mono`, `Roboto`, and `Liberation` TrueType fonts into `/data/local/linux-root/usr/share/fonts/truetype/` and removed `DejaVuMathTeXGyre.ttf`, restoring crisp UI and monospace C++ editor typography.

4. **Agnostic Dynamic App Icon & Title Integration**:

---

## Live Screenshots on Android desktop (`127.0.0.1:6520`)

````carousel
_(screenshot omitted from repo: Real /opt/example-ide/example-ide Wayland app running directly on the Android desktop host with fixed UI/editor fonts, syntax-highlighted wayland_server_engine.cpp, and dynamic the IDE window & dock icons)_
<!-- slide -->
_(screenshot omitted from repo: Live interactive Wayland pointer & keyboard verification: cursor moved to Ln 7, Col 10 and Command Palette opened via F1 inside the direct-on-host the IDE Wayland window)_
````

---

## Runtime & Logcat Verification (`shm_uploads=0`, `dmabuf_imports=298`, `presents=298`)

```text
10-15 06:36:22.534 30985 30985 I VkWlfApp       : Triggered direct-on-host launch of real /opt/example-ide/example-ide connected to /data/user/10/com.google.example.vkwlf/cache/wayland-0 without pKVM
10-15 06:36:22.722 30985 31045 I VulkanRender   : VkRenderer initialized (1152x608, mode=VkSwapchainKHR, ext_mem_fd=0, dmabuf=0, android_ahb=1)
10-15 06:37:09.395 30985 31044 I VkWlfCompositor: xdg_toplevel.set_app_id: surface=3 app_id='example-ide'
10-15 06:37:09.395 30985 31044 I VkWlfCompositor: xdg_toplevel.set_title: surface=3 title='the IDE'
10-15 06:37:13.046 30985 31044 I VkWlfCompositor: Agnostically resolved Linux app icon (343936 bytes, match_score=100, icon='example-ide') for app_id='example-ide' title='the IDE' pid=31070 exe=''
10-15 06:37:13.438 30985 30985 I VkWlfApp       : Committed FabricatedOverlay for dynamic Wayland app icon and label='the IDE' (343936 bytes)
10-15 06:44:46.339 30985 31045 I VulkanRender   : Imported AHardwareBuffer (0x79dee2cd4c70, 1152x608) via VK_ANDROID_external_memory_android_hardware_buffer
10-15 06:44:46.340 30985 31045 I VulkanRender   : Imported Wayland buffer (1152x608, format=0x0) into GPU AHardwareBuffer via VK_ANDROID_external_memory_android_hardware_buffer
10-15 06:44:46.516 30985 31045 I VulkanRender   : Presented frame #298 via vkQueuePresentKHR (swapchainImage=1, surface=7, centerPixel=0xff222222)
10-15 06:44:46.516 30985 31045 I VkWlfCompositor: [HostGPU] Presented Wayland frame #299 (surface=7, buffer_type=DMA-BUF/AHB, shm_uploads=0, dmabuf_imports=298, presents=298)
```
