#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <android/hardware_buffer.h>
#endif
#include <vulkan/vulkan.h>
#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#endif

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "log_macros.h"
#include "xdg-shell-client-protocol.h"

namespace {

constexpr const char* kTag = "WestonSimpleShmClient";

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
         (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}
constexpr uint32_t kDrmFormatARGB8888 = FourCC('A', 'R', '2', '4');

struct ClientState {
  struct wl_display* display = nullptr;
  struct wl_registry* registry = nullptr;
  struct wl_compositor* compositor = nullptr;
  struct wl_subcompositor* subcompositor = nullptr;
  struct wl_data_device_manager* data_device_manager = nullptr;
  struct wl_shm* shm = nullptr;
  struct zwp_linux_dmabuf_v1* dmabuf = nullptr;
  struct xdg_wm_base* wm_base = nullptr;
  struct wl_seat* seat = nullptr;
  struct wl_pointer* pointer = nullptr;
  struct wl_keyboard* keyboard = nullptr;

  struct wl_surface* surface = nullptr;
  struct xdg_surface* xdg_surface = nullptr;
  struct xdg_toplevel* xdg_toplevel = nullptr;

  struct wl_surface* popup_surface = nullptr;
  struct xdg_surface* popup_xdg_surface = nullptr;
  struct xdg_popup* xdg_popup = nullptr;
  bool popup_configured = false;

  int32_t width = 1152;
  int32_t height = 648;
  int32_t pointer_x = 420;
  int32_t pointer_y = 260;
  bool configured = false;
  bool needs_redraw = true;
  uint32_t frames_sent = 0;
  uint32_t pointer_events_received = 0;
  uint32_t key_events_received = 0;
};

// --- wl_pointer listener ---

void PointerEnter(void* data,
                  struct wl_pointer* /*pointer*/,
                  uint32_t serial,
                  struct wl_surface* /*surface*/,
                  wl_fixed_t sx,
                  wl_fixed_t sy) {
  auto* st = static_cast<ClientState*>(data);
  st->pointer_x = static_cast<int32_t>(wl_fixed_to_double(sx));
  st->pointer_y = static_cast<int32_t>(wl_fixed_to_double(sy));
  st->needs_redraw = true;
  LOGI(kTag, "Client received wl_pointer.enter(serial=%u, x=%.1f, y=%.1f)", serial,
       wl_fixed_to_double(sx), wl_fixed_to_double(sy));
}

void PointerLeave(void* /*data*/,
                  struct wl_pointer* /*pointer*/,
                  uint32_t /*serial*/,
                  struct wl_surface* /*surface*/) {}

void PointerMotion(void* data,
                   struct wl_pointer* /*pointer*/,
                   uint32_t time,
                   wl_fixed_t sx,
                   wl_fixed_t sy) {
  auto* st = static_cast<ClientState*>(data);
  st->pointer_x = static_cast<int32_t>(wl_fixed_to_double(sx));
  st->pointer_y = static_cast<int32_t>(wl_fixed_to_double(sy));
  st->pointer_events_received++;
  st->needs_redraw = true;
  LOGI(kTag, "Client received wl_pointer.motion(time=%u, x=%.1f, y=%.1f)", time,
       wl_fixed_to_double(sx), wl_fixed_to_double(sy));
}

void PointerButton(void* data,
                   struct wl_pointer* /*pointer*/,
                   uint32_t serial,
                   uint32_t time,
                   uint32_t button,
                   uint32_t state) {
  auto* st = static_cast<ClientState*>(data);
  st->pointer_events_received++;
  st->needs_redraw = true;
  LOGI(kTag, "Client received wl_pointer.button(serial=%u, time=%u, button=0x%x, state=%u)",
       serial, time, button, state);
}

void PointerAxis(void* /*data*/,
                 struct wl_pointer* /*pointer*/,
                 uint32_t /*time*/,
                 uint32_t /*axis*/,
                 wl_fixed_t /*value*/) {}

void PointerFrame(void* /*data*/, struct wl_pointer* /*pointer*/) {}
void PointerAxisSource(void* /*data*/, struct wl_pointer* /*pointer*/, uint32_t /*source*/) {}
void PointerAxisStop(void* /*data*/,
                     struct wl_pointer* /*pointer*/,
                     uint32_t /*time*/,
                     uint32_t /*axis*/) {}
void PointerAxisDiscrete(void* /*data*/,
                         struct wl_pointer* /*pointer*/,
                         uint32_t /*axis*/,
                         int32_t /*discrete*/) {}

const struct wl_pointer_listener kPointerListener = {
    .enter = PointerEnter,
    .leave = PointerLeave,
    .motion = PointerMotion,
    .button = PointerButton,
    .axis = PointerAxis,
    .frame = PointerFrame,
    .axis_source = PointerAxisSource,
    .axis_stop = PointerAxisStop,
    .axis_discrete = PointerAxisDiscrete,
};

// --- wl_keyboard listener ---

void KeyboardKeymap(void* /*data*/,
                    struct wl_keyboard* /*keyboard*/,
                    uint32_t format,
                    int32_t fd,
                    uint32_t size) {
  LOGI(kTag, "Client received wl_keyboard.keymap(format=%u, fd=%d, size=%u)", format, fd, size);
  if (fd >= 0) close(fd);
}

void KeyboardEnter(void* /*data*/,
                   struct wl_keyboard* /*keyboard*/,
                   uint32_t serial,
                   struct wl_surface* /*surface*/,
                   struct wl_array* /*keys*/) {
  LOGI(kTag, "Client received wl_keyboard.enter(serial=%u)", serial);
}

void KeyboardLeave(void* /*data*/,
                   struct wl_keyboard* /*keyboard*/,
                   uint32_t /*serial*/,
                   struct wl_surface* /*surface*/) {}

void KeyboardKey(void* data,
                 struct wl_keyboard* /*keyboard*/,
                 uint32_t serial,
                 uint32_t time,
                 uint32_t key,
                 uint32_t state) {
  auto* st = static_cast<ClientState*>(data);
  st->key_events_received++;
  LOGI(kTag, "Client received wl_keyboard.key(serial=%u, time=%u, evdev_key=%u, state=%u)", serial,
       time, key, state);
}

void KeyboardModifiers(void* /*data*/,
                       struct wl_keyboard* /*keyboard*/,
                       uint32_t /*serial*/,
                       uint32_t mods_depressed,
                       uint32_t mods_latched,
                       uint32_t mods_locked,
                       uint32_t group) {
  LOGI(kTag,
       "Client received wl_keyboard.modifiers(depressed=0x%x, latched=0x%x, locked=0x%x, group=%u)",
       mods_depressed, mods_latched, mods_locked, group);
}

void KeyboardRepeatInfo(void* /*data*/,
                        struct wl_keyboard* /*keyboard*/,
                        int32_t rate,
                        int32_t delay) {
  LOGI(kTag, "Client received wl_keyboard.repeat_info(rate=%d, delay=%d)", rate, delay);
}

const struct wl_keyboard_listener kKeyboardListener = {
    .keymap = KeyboardKeymap,
    .enter = KeyboardEnter,
    .leave = KeyboardLeave,
    .key = KeyboardKey,
    .modifiers = KeyboardModifiers,
    .repeat_info = KeyboardRepeatInfo,
};

// --- wl_seat listener ---

void SeatCapabilities(void* data, struct wl_seat* seat, uint32_t caps) {
  auto* st = static_cast<ClientState*>(data);
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !st->pointer) {
    st->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(st->pointer, &kPointerListener, st);
  }
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !st->keyboard) {
    st->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(st->keyboard, &kKeyboardListener, st);
  }
}

void SeatName(void* /*data*/, struct wl_seat* /*seat*/, const char* /*name*/) {}

const struct wl_seat_listener kSeatListener = {
    .capabilities = SeatCapabilities,
    .name = SeatName,
};

// --- xdg_wm_base / xdg_surface / xdg_toplevel / xdg_popup listeners ---

void XdgWmBasePing(void* /*data*/, struct xdg_wm_base* wm_base, uint32_t serial) {
  xdg_wm_base_pong(wm_base, serial);
}

const struct xdg_wm_base_listener kWmBaseListener = {
    .ping = XdgWmBasePing,
};

void XdgToplevelConfigure(void* data,
                          struct xdg_toplevel* /*toplevel*/,
                          int32_t width,
                          int32_t height,
                          struct wl_array* /*states*/) {
  auto* st = static_cast<ClientState*>(data);
  if (width > 0 && height > 0) {
    st->width = width;
    st->height = height;
  }
  LOGI(kTag, "Client received xdg_toplevel.configure(%dx%d)", st->width, st->height);
}

void XdgToplevelClose(void* /*data*/, struct xdg_toplevel* /*toplevel*/) {}

const struct xdg_toplevel_listener kToplevelListener = {
    .configure = XdgToplevelConfigure,
    .close = XdgToplevelClose,
};

void XdgSurfaceConfigure(void* data, struct xdg_surface* xdg_surface, uint32_t serial) {
  auto* st = static_cast<ClientState*>(data);
  xdg_surface_ack_configure(xdg_surface, serial);
  st->configured = true;
  st->needs_redraw = true;
}

const struct xdg_surface_listener kXdgSurfaceListener = {
    .configure = XdgSurfaceConfigure,
};

void PopupXdgSurfaceConfigure(void* data, struct xdg_surface* xdg_surface, uint32_t serial) {
  auto* st = static_cast<ClientState*>(data);
  xdg_surface_ack_configure(xdg_surface, serial);
  st->popup_configured = true;
}

const struct xdg_surface_listener kPopupXdgSurfaceListener = {
    .configure = PopupXdgSurfaceConfigure,
};

void XdgPopupConfigure(void* /*data*/,
                       struct xdg_popup* /*popup*/,
                       int32_t x,
                       int32_t y,
                       int32_t width,
                       int32_t height) {
  LOGI(kTag, "Client received xdg_popup.configure(x=%d, y=%d, %dx%d)", x, y, width, height);
}

void XdgPopupDone(void* /*data*/, struct xdg_popup* /*popup*/) {}
void XdgPopupRepositioned(void* /*data*/, struct xdg_popup* /*popup*/, uint32_t /*token*/) {}

const struct xdg_popup_listener kPopupListener = {
    .configure = XdgPopupConfigure,
    .popup_done = XdgPopupDone,
    .repositioned = XdgPopupRepositioned,
};

// --- Registry listener ---

void RegistryGlobal(void* data,
                    struct wl_registry* registry,
                    uint32_t name,
                    const char* interface,
                    uint32_t version) {
  auto* st = static_cast<ClientState*>(data);
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    st->compositor = static_cast<struct wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, 4));
  } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
    st->subcompositor = static_cast<struct wl_subcompositor*>(
        wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
  } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
    st->data_device_manager = static_cast<struct wl_data_device_manager*>(
        wl_registry_bind(registry, name, &wl_data_device_manager_interface,
                         version < 3 ? version : 3));
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    st->shm = static_cast<struct wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
  } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
    st->dmabuf = static_cast<struct zwp_linux_dmabuf_v1*>(
        wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3));
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    st->wm_base = static_cast<struct xdg_wm_base*>(
        wl_registry_bind(registry, name, &xdg_wm_base_interface,
                         version < 3 ? version : 3));
    xdg_wm_base_add_listener(st->wm_base, &kWmBaseListener, st);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    st->seat = static_cast<struct wl_seat*>(
        wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5));
    wl_seat_add_listener(st->seat, &kSeatListener, st);
  }
}

void RegistryGlobalRemove(void* /*data*/, struct wl_registry* /*registry*/, uint32_t /*name*/) {}

const struct wl_registry_listener kRegistryListener = {
    .global = RegistryGlobal,
    .global_remove = RegistryGlobalRemove,
};

void FillAnimatedPattern(uint32_t* pixels, int32_t width, int32_t height, uint32_t frame_idx) {
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      uint8_t r = static_cast<uint8_t>((x + frame_idx * 16) & 0xFF);
      uint8_t g = static_cast<uint8_t>((y + frame_idx * 8) & 0xFF);
      uint8_t b = static_cast<uint8_t>(0x60 + ((frame_idx * 24) & 0x7F));
      // Ensure center region has a deterministic bright color for readback verification
      if (x >= width / 4 && x < (3 * width) / 4 && y >= height / 4 && y < (3 * height) / 4) {
        r = 0x20;
        g = 0x96;
        b = 0xF3;
      }
      pixels[y * width + x] = (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) |
                              (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }
  }
}

#ifdef __ANDROID__
struct NativeHandleHeader {
  int version;
  int numFds;
  int numInts;
  int data[0];
};

extern "C" const NativeHandleHeader* AHardwareBuffer_getNativeHandle(const AHardwareBuffer* buffer);

constexpr uint64_t kAhbHandlePlaneModifier = 0x414842484E444C00ULL;  // 'AHBHNDL\0'
constexpr uint32_t kAhbSerializedMagic = 0x41484248u;                 // 'AHBH'

struct AhbSerializedHeader {
  uint32_t magic = 0;
  AHardwareBuffer_Desc desc{};
  int32_t num_fds = 0;
  int32_t num_ints = 0;
};

struct ClientVulkanGpuRenderer {
  bool initialized = false;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd_buf = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;

  AHardwareBuffer* ahb = nullptr;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  uint32_t current_w = 0;
  uint32_t current_h = 0;

  uint32_t FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((type_filter & (1u << i)) &&
          (mem_props.memoryTypes[i].propertyFlags & props) == props) {
        return i;
      }
    }
    return 0;
  }

  bool Initialize() {
    if (initialized) return true;

    const char* inst_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "VkWlfGpuWaylandClient";
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo inst_info{};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pApplicationInfo = &app_info;
    inst_info.enabledExtensionCount = 2;
    inst_info.ppEnabledExtensionNames = inst_exts;

    if (vkCreateInstance(&inst_info, nullptr, &instance) != VK_SUCCESS) {
      LOGE(kTag, "Client vkCreateInstance failed");
      return false;
    }

    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance, &dev_count, nullptr);
    if (dev_count == 0) return false;
    std::vector<VkPhysicalDevice> devs(dev_count);
    vkEnumeratePhysicalDevices(instance, &dev_count, devs.data());
    physical_device = devs[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_device, &props);
    LOGI(kTag, "Wayland Client Vulkan GPU initialized on PhysicalDevice: '%s'", props.deviceName);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo q_info{};
    q_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q_info.queueFamilyIndex = 0;
    q_info.queueCount = 1;
    q_info.pQueuePriorities = &prio;

    const char* dev_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };

    VkDeviceCreateInfo dev_info{};
    dev_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dev_info.queueCreateInfoCount = 1;
    dev_info.pQueueCreateInfos = &q_info;
    dev_info.enabledExtensionCount = 5;
    dev_info.ppEnabledExtensionNames = dev_exts;

    if (vkCreateDevice(physical_device, &dev_info, nullptr, &device) != VK_SUCCESS) {
      LOGE(kTag, "Client vkCreateDevice failed");
      return false;
    }
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = 0;
    vkCreateCommandPool(device, &pool_info, nullptr, &cmd_pool);

    VkCommandBufferAllocateInfo cb_alloc{};
    cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_alloc.commandPool = cmd_pool;
    cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cb_alloc, &cmd_buf);

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(device, &fence_info, nullptr, &fence);

    VkAttachmentDescription color_att{};
    color_att.format = VK_FORMAT_R8G8B8A8_UNORM;
    color_att.samples = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &color_att;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;

    if (vkCreateRenderPass(device, &rp_info, nullptr, &render_pass) != VK_SUCCESS) {
      return false;
    }

    initialized = true;
    return true;
  }

  bool EnsureTarget(uint32_t w, uint32_t h) {
    if (!Initialize()) return false;
    if (ahb && current_w == w && current_h == h) return true;

    if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
    if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
    if (image != VK_NULL_HANDLE) vkDestroyImage(device, image, nullptr);
    if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
    if (ahb) {
      AHardwareBuffer_release(ahb);
      ahb = nullptr;
    }

    AHardwareBuffer_Desc desc{};
    desc.width = w;
    desc.height = h;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage =
        AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

    if (AHardwareBuffer_allocate(&desc, &ahb) != 0 || !ahb) {
      LOGE(kTag, "Client AHardwareBuffer_allocate(%ux%u) failed", w, h);
      return false;
    }

    auto fpGetAhbProps = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    VkAndroidHardwareBufferPropertiesANDROID ahb_props{};
    ahb_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    if (!fpGetAhbProps || fpGetAhbProps(device, ahb, &ahb_props) != VK_SUCCESS) {
      return false;
    }

    VkExternalMemoryImageCreateInfo ext_info{};
    ext_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.pNext = &ext_info;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent = {w, h, 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &img_info, nullptr, &image) != VK_SUCCESS) return false;

    VkMemoryDedicatedAllocateInfo ded_info{};
    ded_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded_info.image = image;

    VkImportAndroidHardwareBufferInfoANDROID import_ahb{};
    import_ahb.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import_ahb.pNext = &ded_info;
    import_ahb.buffer = ahb;

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &import_ahb;
    alloc_info.allocationSize = ahb_props.allocationSize;
    alloc_info.memoryTypeIndex = FindMemoryType(ahb_props.memoryTypeBits, 0);

    if (vkAllocateMemory(device, &alloc_info, nullptr, &memory) != VK_SUCCESS ||
        vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
      return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) return false;

    VkImageView att[] = {view};
    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = att;
    fb_info.width = w;
    fb_info.height = h;
    fb_info.layers = 1;
    if (vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffer) != VK_SUCCESS) return false;

    current_w = w;
    current_h = h;
    return true;
  }

  static void PushGpuRect(std::vector<VkClearAttachment>* atts,
                          std::vector<VkClearRect>* rects,
                          int32_t x,
                          int32_t y,
                          uint32_t w,
                          uint32_t h,
                          uint32_t max_w,
                          uint32_t max_h,
                          float r,
                          float g,
                          float b) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (static_cast<uint32_t>(x) >= max_w || static_cast<uint32_t>(y) >= max_h) return;
    if (static_cast<uint32_t>(x) + w > max_w) w = max_w - static_cast<uint32_t>(x);
    if (static_cast<uint32_t>(y) + h > max_h) h = max_h - static_cast<uint32_t>(y);
    if (w == 0 || h == 0) return;

    VkClearAttachment ca{};
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = 0;
    ca.clearValue.color = {{r, g, b, 1.0f}};
    atts->push_back(ca);

    VkClearRect cr{};
    cr.rect.offset = {x, y};
    cr.rect.extent = {w, h};
    cr.baseArrayLayer = 0;
    cr.layerCount = 1;
    rects->push_back(cr);
  }

  bool RenderIdeFrameOnGpu(const ClientState* st, uint32_t frame_idx) {
    uint32_t w = static_cast<uint32_t>(std::max(st->width, 64));
    uint32_t h = static_cast<uint32_t>(std::max(st->height, 64));
    if (!EnsureTarget(w, h)) return false;

    vkResetCommandBuffer(cmd_buf, 0);
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd_buf, &begin_info);

    // Base editor background (#1E1E1E)
    VkClearValue clear_val{};
    clear_val.color = {{0.118f, 0.118f, 0.118f, 1.0f}};

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass;
    rp_begin.framebuffer = framebuffer;
    rp_begin.renderArea.offset = {0, 0};
    rp_begin.renderArea.extent = {w, h};
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear_val;

    vkCmdBeginRenderPass(cmd_buf, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    std::vector<VkClearAttachment> atts;
    std::vector<VkClearRect> rects;

    // 1. Left activity bar (#252526) & sidebar (#202021)
    PushGpuRect(&atts, &rects, 0, 0, 48, h, w, h, 0.145f, 0.145f, 0.149f);
    PushGpuRect(&atts, &rects, 0, 44, 3, 36, w, h, 0.0f, 0.478f, 0.8f);  // Active icon indicator
    PushGpuRect(&atts, &rects, 48, 0, 176, h, w, h, 0.125f, 0.125f, 0.130f);

    // Sidebar file items
    for (int i = 0; i < 9; ++i) {
      bool selected = (i == 2);
      if (selected) {
        PushGpuRect(&atts, &rects, 52, 52 + i * 28, 168, 24, w, h, 0.16f, 0.24f, 0.34f);
      }
      PushGpuRect(&atts, &rects, 64, 60 + i * 28, 110 + (i % 3) * 18, 8, w, h,
                  selected ? 0.88f : 0.58f, selected ? 0.92f : 0.60f, selected ? 0.98f : 0.64f);
    }

    // 2. Top tab bar (#2D2D2D) & active tab (#1E1E1E + #007ACC accent)
    PushGpuRect(&atts, &rects, 224, 0, w - 224, 36, w, h, 0.176f, 0.176f, 0.176f);
    PushGpuRect(&atts, &rects, 224, 0, 210, 36, w, h, 0.118f, 0.118f, 0.118f);
    PushGpuRect(&atts, &rects, 224, 0, 210, 3, w, h, 0.0f, 0.478f, 0.8f);
    PushGpuRect(&atts, &rects, 240, 14, 150, 8, w, h, 0.90f, 0.90f, 0.92f);

    // 3. Syntax-highlighted code lines in main editor region (drawn on GPU)
    for (int line = 0; line < 18; ++line) {
      int32_t ly = 56 + line * 26;
      if (static_cast<uint32_t>(ly + 14) >= h - 28) break;
      // Line number gutter
      PushGpuRect(&atts, &rects, 240, ly, 18, 8, w, h, 0.35f, 0.38f, 0.42f);
      int32_t indent = (line % 4 == 0) ? 0 : ((line % 2 == 0) ? 24 : 44);
      // Keyword token (#569CD6)
      PushGpuRect(&atts, &rects, 276 + indent, ly, 56, 9, w, h, 0.337f, 0.612f, 0.839f);
      // Identifier / Function token (#4EC9B0 or #DCDCAA)
      PushGpuRect(&atts, &rects, 342 + indent, ly, 96 + (line % 3) * 32, 9, w, h,
                  (line % 2 == 0) ? 0.306f : 0.863f, (line % 2 == 0) ? 0.788f : 0.863f,
                  (line % 2 == 0) ? 0.690f : 0.667f);
      // String / literal token (#CE9178)
      PushGpuRect(&atts, &rects, 480 + indent, ly, 84 + (line % 4) * 24, 9, w, h, 0.808f, 0.569f,
                  0.471f);
    }

    // Deterministic bright accent panel around center so automated center-pixel verification is non-zero
    PushGpuRect(&atts, &rects, w / 2 - 70, h / 2 - 22, 140, 44, w, h, 0.125f, 0.588f, 0.953f);

    // 4. Interactive GPU pointer & keyboard indicator
    int32_t px = std::clamp(st->pointer_x, 230, static_cast<int32_t>(w) - 40);
    int32_t py = std::clamp(st->pointer_y, 42, static_cast<int32_t>(h) - 48);
    PushGpuRect(&atts, &rects, px - 8, py - 8, 18, 18, w, h, 0.20f, 0.86f, 0.45f);

    // 5. Bottom status bar (#007ACC)
    if (h > 26) {
      PushGpuRect(&atts, &rects, 0, static_cast<int32_t>(h - 24), w, 24, w, h, 0.0f, 0.478f,
                  0.800f);
    }

    for (size_t i = 0; i < atts.size(); ++i) {
      vkCmdClearAttachments(cmd_buf, 1, &atts[i], 1, &rects[i]);
    }

    vkCmdEndRenderPass(cmd_buf);
    vkEndCommandBuffer(cmd_buf);

    vkResetFences(device, 1, &fence);
    VkSubmitInfo sub{};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cmd_buf;
    if (vkQueueSubmit(queue, 1, &sub, fence) != VK_SUCCESS) return false;
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

    LOGI(kTag,
         "Rendered Wayland client frame #%u (%ux%u) on Vulkan GPU into AHardwareBuffer (%p)",
         frame_idx, w, h, static_cast<void*>(ahb));
    return true;
  }
};

ClientVulkanGpuRenderer g_gpu_client;
#endif

struct wl_buffer* CreateShmBufferWithSize(ClientState* st,
                                          int32_t w,
                                          int32_t h,
                                          uint32_t frame_idx) {
  int32_t stride = w * 4;
  size_t size = static_cast<size_t>(stride) * h;

  int fd = memfd_create("weston-simple-shm-buf", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
    if (fd >= 0) close(fd);
    return nullptr;
  }

  auto* data =
      static_cast<uint32_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (data == MAP_FAILED) {
    close(fd);
    return nullptr;
  }

  FillAnimatedPattern(data, w, h, frame_idx);
  munmap(data, size);

  struct wl_shm_pool* pool = wl_shm_create_pool(st->shm, fd, static_cast<int32_t>(size));
  struct wl_buffer* buffer =
      wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  return buffer;
}

struct wl_buffer* CreateShmBuffer(ClientState* st, uint32_t frame_idx) {
  return CreateShmBufferWithSize(st, st->width, st->height, frame_idx);
}

struct wl_buffer* CreateDmaBufBufferWithSize(ClientState* st,
                                             int32_t w,
                                             int32_t h,
                                             uint32_t frame_idx) {
  if (!st->dmabuf) return nullptr;

#ifdef __ANDROID__
  int32_t saved_w = st->width;
  int32_t saved_h = st->height;
  st->width = w;
  st->height = h;
  bool rendered = g_gpu_client.RenderIdeFrameOnGpu(st, frame_idx);
  st->width = saved_w;
  st->height = saved_h;

  if (rendered && g_gpu_client.ahb) {
    const NativeHandleHeader* nh = AHardwareBuffer_getNativeHandle(g_gpu_client.ahb);
    if (nh && nh->numFds >= 1) {
      AHardwareBuffer_Desc actual_desc{};
      AHardwareBuffer_describe(g_gpu_client.ahb, &actual_desc);
      uint32_t byte_stride =
          (actual_desc.stride > 0 ? actual_desc.stride : static_cast<uint32_t>(w)) * 4;

      AhbSerializedHeader hdr{};
      hdr.magic = kAhbSerializedMagic;
      hdr.desc = actual_desc;
      hdr.num_fds = nh->numFds;
      hdr.num_ints = nh->numInts;

      size_t meta_bytes = sizeof(hdr) + static_cast<size_t>(nh->numInts) * sizeof(int);
      int meta_fd = memfd_create("vkwlf-ahb-handle-meta", MFD_CLOEXEC);
      if (meta_fd >= 0 && ftruncate(meta_fd, static_cast<off_t>(meta_bytes)) == 0) {
        pwrite(meta_fd, &hdr, sizeof(hdr), 0);
        if (nh->numInts > 0) {
          pwrite(meta_fd, &nh->data[nh->numFds], static_cast<size_t>(nh->numInts) * sizeof(int),
                 sizeof(hdr));
        }

        struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(st->dmabuf);
        for (int i = 0; i < nh->numFds; ++i) {
          zwp_linux_buffer_params_v1_add(params, nh->data[i], static_cast<uint32_t>(i), 0,
                                         byte_stride, 0, 0);
        }
        uint32_t mod_hi = static_cast<uint32_t>(kAhbHandlePlaneModifier >> 32);
        uint32_t mod_lo = static_cast<uint32_t>(kAhbHandlePlaneModifier & 0xFFFFFFFFu);
        zwp_linux_buffer_params_v1_add(params, meta_fd, static_cast<uint32_t>(nh->numFds), 0,
                                       byte_stride, mod_hi, mod_lo);
        struct wl_buffer* buffer =
            zwp_linux_buffer_params_v1_create_immed(params, w, h, kDrmFormatARGB8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        close(meta_fd);
        return buffer;
      }
      if (meta_fd >= 0) close(meta_fd);
    }
  }
#endif

  int32_t stride = w * 4;
  size_t size = static_cast<size_t>(stride) * h;

  int fd = memfd_create("weston-simple-dmabuf", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
    if (fd >= 0) close(fd);
    return nullptr;
  }

  auto* data =
      static_cast<uint32_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (data == MAP_FAILED) {
    close(fd);
    return nullptr;
  }
  FillAnimatedPattern(data, w, h, frame_idx);
  munmap(data, size);

  struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(st->dmabuf);
  zwp_linux_buffer_params_v1_add(params, fd, 0, 0, static_cast<uint32_t>(stride), 0, 0);
  struct wl_buffer* buffer =
      zwp_linux_buffer_params_v1_create_immed(params, w, h, kDrmFormatARGB8888, 0);
  zwp_linux_buffer_params_v1_destroy(params);
  close(fd);
  return buffer;
}

struct wl_buffer* CreateDmaBufBuffer(ClientState* st, uint32_t frame_idx) {
  return CreateDmaBufBufferWithSize(st, st->width, st->height, frame_idx);
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t total_frames = 6;
  std::string title = "Example IDE - wayland_server_engine.cpp";
  std::string app_id = "example-ide";
  bool with_popup = false;
  bool gpu_only = false;
  bool stay_open = false;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      total_frames = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
      title = argv[++i];
    } else if (strcmp(argv[i], "--app-id") == 0 && i + 1 < argc) {
      app_id = argv[++i];
    } else if (strcmp(argv[i], "--with-popup") == 0) {
      with_popup = true;
    } else if (strcmp(argv[i], "--gpu-only") == 0) {
      gpu_only = true;
    } else if (strcmp(argv[i], "--stay-open") == 0) {
      stay_open = true;
    }
  }

  const char* wl_disp_env = std::getenv("WAYLAND_DISPLAY");
  const char* socket_path = (wl_disp_env && wl_disp_env[0] != '\0') ? wl_disp_env
                                                                    : "/data/local/tmp/wayland-0";

  ClientState st;
  for (int attempt = 0; attempt < 50 && !st.display; ++attempt) {
    st.display = wl_display_connect(socket_path);
    if (!st.display && strcmp(socket_path, "/data/local/tmp/wayland-0") == 0) {
      st.display = wl_display_connect("/tmp/wayland-0");
    }
    if (!st.display) {
      usleep(50000);
    }
  }
  if (!st.display) {
    LOGE(kTag, "Failed to connect to Wayland display '%s'", socket_path);
    return 1;
  }
  LOGI(kTag, "Connected to Wayland compositor at '%s' (gpu_only=%d, stay_open=%d)", socket_path,
       gpu_only ? 1 : 0, stay_open ? 1 : 0);

  st.registry = wl_display_get_registry(st.display);
  wl_registry_add_listener(st.registry, &kRegistryListener, &st);
  wl_display_roundtrip(st.display);
  wl_display_roundtrip(st.display);

  if (!st.compositor || !st.wm_base || (!gpu_only && !st.shm) || (gpu_only && !st.dmabuf)) {
    LOGE(kTag, "Missing required Wayland globals (compositor=%p shm=%p dmabuf=%p wm_base=%p)",
         static_cast<void*>(st.compositor), static_cast<void*>(st.shm),
         static_cast<void*>(st.dmabuf), static_cast<void*>(st.wm_base));
    return 1;
  }

  // Exercise wl_data_device_manager (clipboard initialization used by Electron / VS Code-class IDEs)
  struct wl_data_source* data_source = nullptr;
  struct wl_data_device* data_device = nullptr;
  if (st.data_device_manager && st.seat) {
    data_source = wl_data_device_manager_create_data_source(st.data_device_manager);
    wl_data_source_offer(data_source, "text/plain;charset=utf-8");
    data_device = wl_data_device_manager_get_data_device(st.data_device_manager, st.seat);
    wl_data_device_set_selection(data_device, data_source, 1);
  }

  st.surface = wl_compositor_create_surface(st.compositor);
  st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
  xdg_surface_add_listener(st.xdg_surface, &kXdgSurfaceListener, &st);

  st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
  xdg_toplevel_add_listener(st.xdg_toplevel, &kToplevelListener, &st);
  xdg_toplevel_set_title(st.xdg_toplevel, title.c_str());
  xdg_toplevel_set_app_id(st.xdg_toplevel, app_id.c_str());

  // Initial empty commit to trigger xdg_surface.configure
  wl_surface_commit(st.surface);
  while (!st.configured && wl_display_dispatch(st.display) != -1) {
  }

  // Optionally create an xdg_popup child surface (simulating VS Code-class IDEs Command Palette / Menu)
  if (with_popup) {
    struct xdg_positioner* pos = xdg_wm_base_create_positioner(st.wm_base);
    xdg_positioner_set_size(pos, 240, 120);
    xdg_positioner_set_anchor_rect(pos, 80, 60, 200, 30);
    xdg_positioner_set_offset(pos, 0, 4);

    st.popup_surface = wl_compositor_create_surface(st.compositor);
    st.popup_xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.popup_surface);
    xdg_surface_add_listener(st.popup_xdg_surface, &kPopupXdgSurfaceListener, &st);
    st.xdg_popup = xdg_surface_get_popup(st.popup_xdg_surface, st.xdg_surface, pos);
    xdg_popup_add_listener(st.xdg_popup, &kPopupListener, &st);
    xdg_positioner_destroy(pos);

    wl_surface_commit(st.popup_surface);
    while (!st.popup_configured && wl_display_dispatch(st.display) != -1) {
    }
    struct wl_buffer* popup_buf = gpu_only ? CreateDmaBufBufferWithSize(&st, 240, 120, 10)
                                           : CreateShmBufferWithSize(&st, 240, 120, 10);
    if (popup_buf) {
      wl_surface_attach(st.popup_surface, popup_buf, 0, 0);
      wl_surface_damage(st.popup_surface, 0, 0, 240, 120);
      wl_surface_commit(st.popup_surface);
      wl_display_roundtrip(st.display);
      wl_buffer_destroy(popup_buf);
    }
  }

  for (uint32_t i = 0; stay_open || i < total_frames; ++i) {
    if (with_popup && !stay_open && i == total_frames / 2 && st.xdg_popup) {
      xdg_popup_destroy(st.xdg_popup);
      st.xdg_popup = nullptr;
      if (st.popup_xdg_surface) {
        xdg_surface_destroy(st.popup_xdg_surface);
        st.popup_xdg_surface = nullptr;
      }
      if (st.popup_surface) {
        wl_surface_destroy(st.popup_surface);
        st.popup_surface = nullptr;
      }
      wl_display_roundtrip(st.display);
      LOGI(kTag, "Closed xdg_popup at frame %u to verify popup unmap and focus preservation", i);
    }

    bool use_dmabuf = (gpu_only || (i % 2 == 1)) && (st.dmabuf != nullptr);
    struct wl_buffer* buf =
        use_dmabuf ? CreateDmaBufBuffer(&st, i) : CreateShmBuffer(&st, i);
    if (!buf) {
      LOGE(kTag, "Failed to allocate client wl_buffer for frame %u", i);
      break;
    }

    struct wl_callback* frame_cb = wl_surface_frame(st.surface);
    bool frame_done = false;
    static const struct wl_callback_listener kFrameListener = {
        .done = [](void* data, struct wl_callback* cb, uint32_t /*time*/) {
          *static_cast<bool*>(data) = true;
          wl_callback_destroy(cb);
        },
    };
    wl_callback_add_listener(frame_cb, &kFrameListener, &frame_done);

    wl_surface_attach(st.surface, buf, 0, 0);
    wl_surface_damage(st.surface, 0, 0, st.width, st.height);
    wl_surface_commit(st.surface);
    st.frames_sent++;
    st.needs_redraw = false;

    while (!frame_done && wl_display_dispatch(st.display) != -1) {
    }
    usleep(20000);
    wl_display_roundtrip(st.display);
    wl_buffer_destroy(buf);

    if (stay_open && st.frames_sent >= 4) {
      // Wait for input/configure events before rendering the next GPU frame
      while (!st.needs_redraw) {
        if (wl_display_dispatch(st.display) == -1) {
          stay_open = false;
          break;
        }
      }
    }
  }

  // Final roundtrip to receive any queued input events
  wl_display_roundtrip(st.display);
  LOGI(kTag,
       "Client completed: frames_sent=%u, pointer_events_received=%u, key_events_received=%u",
       st.frames_sent, st.pointer_events_received, st.key_events_received);

  if (st.xdg_popup) xdg_popup_destroy(st.xdg_popup);
  if (st.popup_xdg_surface) xdg_surface_destroy(st.popup_xdg_surface);
  if (st.popup_surface) wl_surface_destroy(st.popup_surface);
  if (data_device) wl_data_device_release(data_device);
  if (data_source) wl_data_source_destroy(data_source);
  if (st.xdg_toplevel) xdg_toplevel_destroy(st.xdg_toplevel);
  if (st.xdg_surface) xdg_surface_destroy(st.xdg_surface);
  if (st.surface) wl_surface_destroy(st.surface);
  wl_display_disconnect(st.display);
  return 0;
}
