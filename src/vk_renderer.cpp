#include "vk_renderer.h"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include "log_macros.h"

namespace vkwlf {

namespace {

constexpr const char* kTag = "VulkanRender";

// Pre-assembled, spec-compliant SPIR-V 1.0 Vertex Shader:
//   #version 450
//   layout(location = 0) in vec2 inPos;
//   layout(location = 1) in vec2 inUV;
//   layout(location = 0) out vec2 outUV;
//   void main() {
//     gl_Position = vec4(inPos.x, inPos.y, 0.0, 1.0);
//     outUV = inUV;
//   }
constexpr uint32_t kQuadVertSpirv[] = {
    0x07230203, 0x00010000, 0x00000000, 0x00000018, 0x00000000,
    // OpCapability Shader
    0x00020011, 0x00000001,
    // OpMemoryModel Logical GLSL450
    0x0003000e, 0x00000000, 0x00000001,
    // OpEntryPoint Vertex %1 "main" %2 %3 %4 %5
    0x0009000f, 0x00000000, 0x00000001, 0x6e69616d, 0x00000000,
    0x00000002, 0x00000003, 0x00000004, 0x00000005,
    // OpDecorate %2(inPos) Location 0
    0x00040047, 0x00000002, 0x0000001e, 0x00000000,
    // OpDecorate %3(inUV) Location 1
    0x00040047, 0x00000003, 0x0000001e, 0x00000001,
    // OpDecorate %4(gl_Position) BuiltIn Position
    0x00040047, 0x00000004, 0x0000000b, 0x00000000,
    // OpDecorate %5(outUV) Location 0
    0x00040047, 0x00000005, 0x0000001e, 0x00000000,
    // %6 = OpTypeVoid
    0x00020013, 0x00000006,
    // %7 = OpTypeFunction %6
    0x00030021, 0x00000007, 0x00000006,
    // %8 = OpTypeFloat 32
    0x00030016, 0x00000008, 0x00000020,
    // %9 = OpTypeVector %8 2
    0x00040017, 0x00000009, 0x00000008, 0x00000002,
    // %10 = OpTypeVector %8 4
    0x00040017, 0x0000000a, 0x00000008, 0x00000004,
    // %11 = OpTypePointer Input %9
    0x00040020, 0x0000000b, 0x00000001, 0x00000009,
    // %12 = OpTypePointer Output %10
    0x00040020, 0x0000000c, 0x00000003, 0x0000000a,
    // %13 = OpTypePointer Output %9
    0x00040020, 0x0000000d, 0x00000003, 0x00000009,
    // %2 = OpVariable %11 Input (inPos)
    0x0004003b, 0x0000000b, 0x00000002, 0x00000001,
    // %3 = OpVariable %11 Input (inUV)
    0x0004003b, 0x0000000b, 0x00000003, 0x00000001,
    // %4 = OpVariable %12 Output (gl_Position)
    0x0004003b, 0x0000000c, 0x00000004, 0x00000003,
    // %5 = OpVariable %13 Output (outUV)
    0x0004003b, 0x0000000d, 0x00000005, 0x00000003,
    // %14 = OpConstant %8 0.0f
    0x0004002b, 0x00000008, 0x0000000e, 0x00000000,
    // %15 = OpConstant %8 1.0f
    0x0004002b, 0x00000008, 0x0000000f, 0x3f800000,
    // %1 = OpFunction %6 None %7
    0x00050036, 0x00000006, 0x00000001, 0x00000000, 0x00000007,
    // %16 = OpLabel
    0x000200f8, 0x00000010,
    // %17 = OpLoad %9 %2
    0x0004003d, 0x00000009, 0x00000011, 0x00000002,
    // %18 = OpCompositeExtract %8 %17 0
    0x00050051, 0x00000008, 0x00000012, 0x00000011, 0x00000000,
    // %19 = OpCompositeExtract %8 %17 1
    0x00050051, 0x00000008, 0x00000013, 0x00000011, 0x00000001,
    // %20 = OpCompositeConstruct %10 %18 %19 %14 %15
    0x00070050, 0x0000000a, 0x00000014, 0x00000012, 0x00000013,
    0x0000000e, 0x0000000f,
    // OpStore %4 %20
    0x0003003e, 0x00000004, 0x00000014,
    // %21 = OpLoad %9 %3
    0x0004003d, 0x00000009, 0x00000015, 0x00000003,
    // OpStore %5 %21
    0x0003003e, 0x00000005, 0x00000015,
    // OpReturn
    0x000100fd,
    // OpFunctionEnd
    0x00010038,
};

// Pre-assembled, spec-compliant SPIR-V 1.0 Fragment Shader:
//   #version 450
//   layout(set = 0, binding = 0) uniform sampler2D texSampler;
//   layout(location = 0) in vec2 inUV;
//   layout(location = 0) out vec4 outColor;
//   void main() {
//     outColor = texture(texSampler, inUV);
//   }
constexpr uint32_t kQuadFragSpirv[] = {
    0x07230203, 0x00010000, 0x00000000, 0x00000013, 0x00000000,
    // OpCapability Shader
    0x00020011, 0x00000001,
    // OpMemoryModel Logical GLSL450
    0x0003000e, 0x00000000, 0x00000001,
    // OpEntryPoint Fragment %1 "main" %2 %3
    0x0007000f, 0x00000004, 0x00000001, 0x6e69616d, 0x00000000,
    0x00000002, 0x00000003,
    // OpExecutionMode %1 OriginUpperLeft
    0x00030010, 0x00000001, 0x00000007,
    // OpDecorate %2(inUV) Location 0
    0x00040047, 0x00000002, 0x0000001e, 0x00000000,
    // OpDecorate %3(outColor) Location 0
    0x00040047, 0x00000003, 0x0000001e, 0x00000000,
    // OpDecorate %4(texSampler) DescriptorSet 0
    0x00040047, 0x00000004, 0x00000022, 0x00000000,
    // OpDecorate %4(texSampler) Binding 0
    0x00040047, 0x00000004, 0x00000021, 0x00000000,
    // %5 = OpTypeVoid
    0x00020013, 0x00000005,
    // %6 = OpTypeFunction %5
    0x00030021, 0x00000006, 0x00000005,
    // %7 = OpTypeFloat 32
    0x00030016, 0x00000007, 0x00000020,
    // %8 = OpTypeVector %7 2
    0x00040017, 0x00000008, 0x00000007, 0x00000002,
    // %9 = OpTypeVector %7 4
    0x00040017, 0x00000009, 0x00000007, 0x00000004,
    // %10 = OpTypeImage %7 2D 0 0 0 1 Unknown
    0x00090019, 0x0000000a, 0x00000007, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0x00000000,
    // %11 = OpTypeSampledImage %10
    0x0003001b, 0x0000000b, 0x0000000a,
    // %12 = OpTypePointer UniformConstant %11
    0x00040020, 0x0000000c, 0x00000000, 0x0000000b,
    // %13 = OpTypePointer Input %8
    0x00040020, 0x0000000d, 0x00000001, 0x00000008,
    // %14 = OpTypePointer Output %9
    0x00040020, 0x0000000e, 0x00000003, 0x00000009,
    // %2 = OpVariable %13 Input (inUV)
    0x0004003b, 0x0000000d, 0x00000002, 0x00000001,
    // %3 = OpVariable %14 Output (outColor)
    0x0004003b, 0x0000000e, 0x00000003, 0x00000003,
    // %4 = OpVariable %12 UniformConstant (texSampler)
    0x0004003b, 0x0000000c, 0x00000004, 0x00000000,
    // %1 = OpFunction %5 None %6
    0x00050036, 0x00000005, 0x00000001, 0x00000000, 0x00000006,
    // %15 = OpLabel
    0x000200f8, 0x0000000f,
    // %16 = OpLoad %11 %4
    0x0004003d, 0x0000000b, 0x00000010, 0x00000004,
    // %17 = OpLoad %8 %2
    0x0004003d, 0x00000008, 0x00000011, 0x00000002,
    // %18 = OpImageSampleImplicitLod %9 %16 %17
    0x00050057, 0x00000009, 0x00000012, 0x00000010, 0x00000011,
    // OpStore %3 %18
    0x0003003e, 0x00000003, 0x00000012,
    // OpReturn
    0x000100fd,
    // OpFunctionEnd
    0x00010038,
};

bool HasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
  for (const auto& ext : exts) {
    if (strcmp(ext.extensionName, name) == 0) return true;
  }
  return false;
}

}  // namespace

VkRenderer::VkRenderer(const Config& config) : config_(config) {
  swapchain_extent_ = {config.width, config.height};
}

VkRenderer::~VkRenderer() { Shutdown(); }

bool VkRenderer::Initialize(ANativeWindow* native_window) {
  if (initialized_) return true;

#ifdef __ANDROID__
  if (native_window) {
    int32_t w = ANativeWindow_getWidth(native_window);
    int32_t h = ANativeWindow_getHeight(native_window);
    if (w > 0 && h > 0) {
      config_.width = static_cast<uint32_t>(w);
      config_.height = static_cast<uint32_t>(h);
      swapchain_extent_ = {config_.width, config_.height};
    }
  }
#endif

  if (!CreateInstance(native_window != nullptr)) return false;
  if (!CreateSurface(native_window)) return false;
  if (!PickPhysicalDevice()) return false;
  if (!CreateLogicalDevice()) return false;
  if (!CreateCommandResources()) return false;
  if (!CreateSwapchainOrOffscreenTarget()) return false;
  if (!CreateRenderPass()) return false;
  if (!CreateDescriptorResources()) return false;
  if (!CreateGraphicsPipeline()) return false;
  if (!CreateFramebuffers()) return false;
  if (!CreateVertexBuffer()) return false;

  initialized_ = true;
  LOGI(kTag,
       "VkRenderer initialized (%ux%u, mode=%s, ext_mem_fd=%d, dmabuf=%d, android_ahb=%d)",
       swapchain_extent_.width, swapchain_extent_.height,
       use_swapchain_ ? "VkSwapchainKHR" : "OffscreenVkImage", has_external_memory_fd_,
       has_dma_buf_ext_, has_android_ahb_ext_);
  return true;
}

bool VkRenderer::CreateInstance(bool has_android_window) {
  uint32_t ext_count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> avail_exts(ext_count);
  vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, avail_exts.data());

  std::vector<const char*> enabled_exts;
  if (HasExtension(avail_exts, VK_KHR_SURFACE_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  }
#ifdef __ANDROID__
  if (has_android_window && HasExtension(avail_exts, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
  }
#else
  (void)has_android_window;
  if (HasExtension(avail_exts, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
  }
#endif
  if (HasExtension(avail_exts, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
  }
  if (HasExtension(avail_exts, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
  }

  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "VkWlfApp";
  app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.pEngineName = "VkWlfCompositorEngine";
  app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;
  create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_exts.size());
  create_info.ppEnabledExtensionNames = enabled_exts.data();

  VkResult res = vkCreateInstance(&create_info, nullptr, &instance_);
  if (res != VK_SUCCESS) {
    LOGE(kTag, "vkCreateInstance failed: %d", res);
    return false;
  }
  return true;
}

bool VkRenderer::CreateSurface(ANativeWindow* native_window) {
#ifdef __ANDROID__
  if (native_window) {
    VkAndroidSurfaceCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    create_info.window = native_window;
    VkResult res = vkCreateAndroidSurfaceKHR(instance_, &create_info, nullptr, &surface_);
    if (res != VK_SUCCESS) {
      LOGE(kTag, "vkCreateAndroidSurfaceKHR failed: %d", res);
      return false;
    }
    LOGI(kTag, "Created VkSurfaceKHR bound to ANativeWindow (%p)",
         static_cast<void*>(native_window));
    return true;
  }
#else
  (void)native_window;
  auto fpCreateHeadlessSurfaceEXT = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
      vkGetInstanceProcAddr(instance_, "vkCreateHeadlessSurfaceEXT"));
  if (fpCreateHeadlessSurfaceEXT) {
    VkHeadlessSurfaceCreateInfoEXT create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    if (fpCreateHeadlessSurfaceEXT(instance_, &create_info, nullptr, &surface_) == VK_SUCCESS) {
      LOGI(kTag, "Created VK_EXT_headless_surface VkSurfaceKHR for swapchain presentation");
      return true;
    }
  }
#endif
  LOGI(kTag, "No WSI surface bound; using offscreen presentation target");
  return true;
}

bool VkRenderer::PickPhysicalDevice() {
  uint32_t dev_count = 0;
  vkEnumeratePhysicalDevices(instance_, &dev_count, nullptr);
  if (dev_count == 0) {
    LOGE(kTag, "No Vulkan physical devices found");
    return false;
  }
  std::vector<VkPhysicalDevice> devices(dev_count);
  vkEnumeratePhysicalDevices(instance_, &dev_count, devices.data());

  for (int pass = 0; pass < 2 && physical_device_ == VK_NULL_HANDLE; ++pass) {
    for (VkPhysicalDevice dev : devices) {
      VkPhysicalDeviceProperties dev_props{};
      vkGetPhysicalDeviceProperties(dev, &dev_props);
      // Pass 0: prefer hardware/virtual GPUs (Intel Xe, Gfxstream, Virtio-GPU Venus) over CPU ICDs.
      if (pass == 0 && dev_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        continue;
      }
      uint32_t q_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(dev, &q_count, nullptr);
      std::vector<VkQueueFamilyProperties> q_props(q_count);
      vkGetPhysicalDeviceQueueFamilyProperties(dev, &q_count, q_props.data());

      for (uint32_t i = 0; i < q_count; ++i) {
        if (q_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
          physical_device_ = dev;
          graphics_queue_family_ = i;
          break;
        }
      }
      if (physical_device_ != VK_NULL_HANDLE) break;
    }
  }

  if (physical_device_ == VK_NULL_HANDLE) {
    LOGE(kTag, "Failed to find a Vulkan physical device with a graphics queue");
    return false;
  }

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical_device_, &props);
  LOGI(kTag, "Selected Vulkan PhysicalDevice: '%s' (API %u.%u.%u, queueFamily=%u)",
       props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
       VK_VERSION_PATCH(props.apiVersion), graphics_queue_family_);
  return true;
}

bool VkRenderer::CreateLogicalDevice() {
  uint32_t ext_count = 0;
  vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> avail_exts(ext_count);
  vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, avail_exts.data());

  std::vector<const char*> enabled_exts;
  if (surface_ != VK_NULL_HANDLE && HasExtension(avail_exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    use_swapchain_ = true;
  }
  if (HasExtension(avail_exts, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
  }
  if (HasExtension(avail_exts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    has_external_memory_fd_ = true;
  }
  if (HasExtension(avail_exts, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    has_dma_buf_ext_ = true;
  }
  if (HasExtension(avail_exts, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    has_external_memory_host_ = true;
  }
  if (HasExtension(avail_exts, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
  }
  if (HasExtension(avail_exts, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
  }
#ifdef __ANDROID__
  if (HasExtension(avail_exts,
                   VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    has_android_ahb_ext_ = true;
  }
  if (HasExtension(avail_exts, VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME);
  }
  if (HasExtension(avail_exts, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
    enabled_exts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
  }
#endif

  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = graphics_queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkDeviceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  create_info.queueCreateInfoCount = 1;
  create_info.pQueueCreateInfos = &queue_info;
  create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_exts.size());
  create_info.ppEnabledExtensionNames = enabled_exts.data();

  VkResult res = vkCreateDevice(physical_device_, &create_info, nullptr, &device_);
  if (res != VK_SUCCESS) {
    LOGE(kTag, "vkCreateDevice failed: %d", res);
    return false;
  }

  vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
  present_queue_ = graphics_queue_;
  return true;
}

bool VkRenderer::CreateSwapchainOrOffscreenTarget() {
  if (use_swapchain_ && surface_ != VK_NULL_HANDLE) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps) ==
        VK_SUCCESS) {
      if (caps.currentExtent.width != 0xFFFFFFFF && caps.currentExtent.width > 0 &&
          caps.currentExtent.height > 0) {
        swapchain_extent_ = caps.currentExtent;
      } else {
        swapchain_extent_.width = std::clamp(config_.width, caps.minImageExtent.width,
                                             caps.maxImageExtent.width);
        swapchain_extent_.height = std::clamp(config_.height, caps.minImageExtent.height,
                                              caps.maxImageExtent.height);
      }

      uint32_t fmt_count = 0;
      vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, nullptr);
      std::vector<VkSurfaceFormatKHR> formats(fmt_count);
      if (fmt_count > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count,
                                             formats.data());
        swapchain_format_ = formats[0].format;
        for (const auto& f : formats) {
          if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            swapchain_format_ = f.format;
            break;
          }
        }
      }

      uint32_t img_count = std::max(caps.minImageCount, 2u);
      if (caps.maxImageCount > 0 && img_count > caps.maxImageCount) {
        img_count = caps.maxImageCount;
      }

      VkSwapchainCreateInfoKHR sc_info{};
      sc_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
      sc_info.surface = surface_;
      sc_info.minImageCount = img_count;
      sc_info.imageFormat = swapchain_format_;
      sc_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      sc_info.imageExtent = swapchain_extent_;
      sc_info.imageArrayLayers = 1;
      sc_info.imageUsage =
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      sc_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
      sc_info.preTransform = (caps.currentTransform != 0)
                                 ? caps.currentTransform
                                 : VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
      sc_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      if ((caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
          sc_info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        }
      }
      sc_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
      sc_info.clipped = VK_TRUE;

      if (vkCreateSwapchainKHR(device_, &sc_info, nullptr, &swapchain_) == VK_SUCCESS) {
        uint32_t sc_img_count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &sc_img_count, nullptr);
        swapchain_images_.resize(sc_img_count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &sc_img_count, swapchain_images_.data());

        swapchain_image_views_.resize(sc_img_count);
        for (uint32_t i = 0; i < sc_img_count; ++i) {
          VkImageViewCreateInfo view_info{};
          view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
          view_info.image = swapchain_images_[i];
          view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
          view_info.format = swapchain_format_;
          view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          view_info.subresourceRange.baseMipLevel = 0;
          view_info.subresourceRange.levelCount = 1;
          view_info.subresourceRange.baseArrayLayer = 0;
          view_info.subresourceRange.layerCount = 1;
          if (vkCreateImageView(device_, &view_info, nullptr, &swapchain_image_views_[i]) !=
              VK_SUCCESS) {
            LOGE(kTag, "Failed to create swapchain VkImageView %u", i);
            return false;
          }
        }
        LOGI(kTag, "Created VkSwapchainKHR (%u images, %ux%u, format=%d)", sc_img_count,
             swapchain_extent_.width, swapchain_extent_.height, swapchain_format_);
      } else {
        use_swapchain_ = false;
      }
    } else {
      use_swapchain_ = false;
    }
  }

  if (!use_swapchain_) {
    swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    swapchain_extent_ = {config_.width, config_.height};

    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = swapchain_format_;
    img_info.extent = {swapchain_extent_.width, swapchain_extent_.height, 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &img_info, nullptr, &offscreen_image_) != VK_SUCCESS) {
      LOGE(kTag, "Failed to create offscreen target VkImage");
      return false;
    }
    VkMemoryRequirements mem_reqs{};
    vkGetImageMemoryRequirements(device_, offscreen_image_, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex =
        FindMemoryType(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &offscreen_memory_) != VK_SUCCESS ||
        vkBindImageMemory(device_, offscreen_image_, offscreen_memory_, 0) != VK_SUCCESS) {
      LOGE(kTag, "Failed to allocate/bind offscreen target memory");
      return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = offscreen_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = swapchain_format_;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &offscreen_view_) != VK_SUCCESS) {
      LOGE(kTag, "Failed to create offscreen VkImageView");
      return false;
    }
    swapchain_images_ = {offscreen_image_};
    swapchain_image_views_ = {offscreen_view_};
  }

  // Create a readback buffer so we can sample pixels and dump rendered swapchain frames.
  VkBufferCreateInfo rb_info{};
  rb_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
#ifdef __ANDROID__
  rb_info.size = use_swapchain_
                     ? sizeof(uint32_t)
                     : static_cast<VkDeviceSize>(swapchain_extent_.width) *
                           swapchain_extent_.height * 4;
#else
  rb_info.size = static_cast<VkDeviceSize>(swapchain_extent_.width) * swapchain_extent_.height * 4;
#endif
  rb_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  rb_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &rb_info, nullptr, &readback_buffer_) == VK_SUCCESS) {
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, readback_buffer_, &reqs);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = FindMemoryType(
        reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &alloc, nullptr, &readback_memory_) == VK_SUCCESS) {
      vkBindBufferMemory(device_, readback_buffer_, readback_memory_, 0);
    }
  }

  return true;
}

bool VkRenderer::CreateRenderPass() {
  VkAttachmentDescription color_attachment{};
  color_attachment.format = swapchain_format_;
  color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color_attachment.finalLayout =
      use_swapchain_ ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  VkAttachmentReference color_ref{};
  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;

  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.srcAccessMask = 0;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo rp_info{};
  rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp_info.attachmentCount = 1;
  rp_info.pAttachments = &color_attachment;
  rp_info.subpassCount = 1;
  rp_info.pSubpasses = &subpass;
  rp_info.dependencyCount = 1;
  rp_info.pDependencies = &dep;

  if (vkCreateRenderPass(device_, &rp_info, nullptr, &render_pass_) != VK_SUCCESS) {
    LOGE(kTag, "vkCreateRenderPass failed");
    return false;
  }
  return true;
}

bool VkRenderer::CreateDescriptorResources() {
  VkSamplerCreateInfo samp_info{};
  samp_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samp_info.magFilter = VK_FILTER_LINEAR;
  samp_info.minFilter = VK_FILTER_LINEAR;
  samp_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  samp_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

  if (vkCreateSampler(device_, &samp_info, nullptr, &sampler_) != VK_SUCCESS) {
    LOGE(kTag, "vkCreateSampler failed");
    return false;
  }

  VkDescriptorSetLayoutBinding sampler_binding{};
  sampler_binding.binding = 0;
  sampler_binding.descriptorCount = 1;
  sampler_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  sampler_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &sampler_binding;

  if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout_) !=
      VK_SUCCESS) {
    LOGE(kTag, "vkCreateDescriptorSetLayout failed");
    return false;
  }

  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_size.descriptorCount = 256;

  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  pool_info.maxSets = 256;

  if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
    LOGE(kTag, "vkCreateDescriptorPool failed");
    return false;
  }
  return true;
}

bool VkRenderer::CreateGraphicsPipeline() {
  VkShaderModuleCreateInfo vert_info{};
  vert_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  vert_info.codeSize = sizeof(kQuadVertSpirv);
  vert_info.pCode = kQuadVertSpirv;

  VkShaderModule vert_mod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &vert_info, nullptr, &vert_mod) != VK_SUCCESS) {
    LOGE(kTag, "Failed to create vertex VkShaderModule");
    return false;
  }

  VkShaderModuleCreateInfo frag_info{};
  frag_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  frag_info.codeSize = sizeof(kQuadFragSpirv);
  frag_info.pCode = kQuadFragSpirv;

  VkShaderModule frag_mod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &frag_info, nullptr, &frag_mod) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, vert_mod, nullptr);
    LOGE(kTag, "Failed to create fragment VkShaderModule");
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert_mod;
  stages[0].pName = "main";

  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag_mod;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding_desc{};
  binding_desc.binding = 0;
  binding_desc.stride = sizeof(Vertex);
  binding_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  VkVertexInputAttributeDescription attr_descs[2]{};
  attr_descs[0].binding = 0;
  attr_descs[0].location = 0;
  attr_descs[0].format = VK_FORMAT_R32G32_SFLOAT;
  attr_descs[0].offset = offsetof(Vertex, pos);

  attr_descs[1].binding = 0;
  attr_descs[1].location = 1;
  attr_descs[1].format = VK_FORMAT_R32G32_SFLOAT;
  attr_descs[1].offset = offsetof(Vertex, uv);

  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding_desc;
  vertex_input.vertexAttributeDescriptionCount = 2;
  vertex_input.pVertexAttributeDescriptions = attr_descs;

  VkPipelineInputAssemblyStateCreateInfo input_assembly{};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport_state{};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blend_attachment.blendEnable = VK_TRUE;
  blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
  blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;

  VkPipelineColorBlendStateCreateInfo color_blending{};
  color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blending.attachmentCount = 1;
  color_blending.pAttachments = &blend_attachment;

  VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn_state{};
  dyn_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dyn_state.dynamicStateCount = 2;
  dyn_state.pDynamicStates = dyn_states;

  VkPipelineLayoutCreateInfo pl_info{};
  pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl_info.setLayoutCount = 1;
  pl_info.pSetLayouts = &descriptor_set_layout_;

  if (vkCreatePipelineLayout(device_, &pl_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, frag_mod, nullptr);
    vkDestroyShaderModule(device_, vert_mod, nullptr);
    LOGE(kTag, "vkCreatePipelineLayout failed");
    return false;
  }

  VkGraphicsPipelineCreateInfo pipe_info{};
  pipe_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipe_info.stageCount = 2;
  pipe_info.pStages = stages;
  pipe_info.pVertexInputState = &vertex_input;
  pipe_info.pInputAssemblyState = &input_assembly;
  pipe_info.pViewportState = &viewport_state;
  pipe_info.pRasterizationState = &rasterizer;
  pipe_info.pMultisampleState = &multisampling;
  pipe_info.pColorBlendState = &color_blending;
  pipe_info.pDynamicState = &dyn_state;
  pipe_info.layout = pipeline_layout_;
  pipe_info.renderPass = render_pass_;
  pipe_info.subpass = 0;

  VkResult res =
      vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipe_info, nullptr, &graphics_pipeline_);
  vkDestroyShaderModule(device_, frag_mod, nullptr);
  vkDestroyShaderModule(device_, vert_mod, nullptr);

  if (res != VK_SUCCESS) {
    LOGE(kTag, "vkCreateGraphicsPipelines failed: %d", res);
    return false;
  }
  return true;
}

bool VkRenderer::CreateFramebuffers() {
  framebuffers_.resize(swapchain_image_views_.size());
  for (size_t i = 0; i < swapchain_image_views_.size(); ++i) {
    VkImageView attachments[] = {swapchain_image_views_[i]};
    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass_;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = attachments;
    fb_info.width = swapchain_extent_.width;
    fb_info.height = swapchain_extent_.height;
    fb_info.layers = 1;
    if (vkCreateFramebuffer(device_, &fb_info, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
      LOGE(kTag, "vkCreateFramebuffer failed for index %zu", i);
      return false;
    }
  }
  return true;
}

bool VkRenderer::CreateCommandResources() {
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = graphics_queue_family_;

  if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
    LOGE(kTag, "vkCreateCommandPool failed");
    return false;
  }

  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = command_pool_;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;

  if (vkAllocateCommandBuffers(device_, &alloc_info, &render_cmd_buffer_) != VK_SUCCESS) {
    LOGE(kTag, "vkAllocateCommandBuffers failed");
    return false;
  }

  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  if (vkCreateSemaphore(device_, &sem_info, nullptr, &image_available_sem_) != VK_SUCCESS ||
      vkCreateSemaphore(device_, &sem_info, nullptr, &render_finished_sem_) != VK_SUCCESS ||
      vkCreateFence(device_, &fence_info, nullptr, &in_flight_fence_) != VK_SUCCESS) {
    LOGE(kTag, "Failed to create synchronization primitives");
    return false;
  }
  return true;
}

bool VkRenderer::CreateVertexBuffer() {
  VkDeviceSize size = sizeof(Vertex) * 6;
  VkBufferCreateInfo buf_info{};
  buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buf_info.size = size;
  buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateBuffer(device_, &buf_info, nullptr, &vertex_buffer_) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements reqs{};
  vkGetBufferMemoryRequirements(device_, vertex_buffer_, &reqs);

  VkMemoryAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = reqs.size;
  alloc_info.memoryTypeIndex = FindMemoryType(
      reqs.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  if (vkAllocateMemory(device_, &alloc_info, nullptr, &vertex_memory_) != VK_SUCCESS ||
      vkBindBufferMemory(device_, vertex_buffer_, vertex_memory_, 0) != VK_SUCCESS) {
    return false;
  }

  return UpdateQuadVertices(0, 0, swapchain_extent_.width, swapchain_extent_.height);
}

bool VkRenderer::UpdateQuadVertices(int32_t x, int32_t y, uint32_t w, uint32_t h) {
  float sw = static_cast<float>(std::max(swapchain_extent_.width, 1u));
  float sh = static_cast<float>(std::max(swapchain_extent_.height, 1u));

  // If the client surface is smaller than the display and at (0,0), center it or place at (x,y);
  // if it matches or exceeds display, draw fullscreen.
  float x0 = -1.0f;
  float y0 = -1.0f;
  float x1 = 1.0f;
  float y1 = 1.0f;
  if (x != 0 || y != 0) {
    x0 = (2.0f * static_cast<float>(x) / sw) - 1.0f;
    y0 = (2.0f * static_cast<float>(y) / sh) - 1.0f;
    x1 = (2.0f * static_cast<float>(x + static_cast<int32_t>(w)) / sw) - 1.0f;
    y1 = (2.0f * static_cast<float>(y + static_cast<int32_t>(h)) / sh) - 1.0f;
  }

  const Vertex vertices[6] = {
      {{x0, y0}, {0.0f, 0.0f}}, {{x1, y0}, {1.0f, 0.0f}}, {{x1, y1}, {1.0f, 1.0f}},
      {{x0, y0}, {0.0f, 0.0f}}, {{x1, y1}, {1.0f, 1.0f}}, {{x0, y1}, {0.0f, 1.0f}},
  };

  void* mapped = nullptr;
  if (vkMapMemory(device_, vertex_memory_, 0, sizeof(vertices), 0, &mapped) != VK_SUCCESS) {
    return false;
  }
  memcpy(mapped, vertices, sizeof(vertices));
  vkUnmapMemory(device_, vertex_memory_);
  return true;
}

uint32_t VkRenderer::FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) const {
  VkPhysicalDeviceMemoryProperties mem_props{};
  vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((type_filter & (1u << i)) &&
        (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if (type_filter & (1u << i)) return i;
  }
  return 0;
}

VkCommandBuffer VkRenderer::BeginSingleTimeCommands() {
  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandPool = command_pool_;
  alloc_info.commandBufferCount = 1;

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device_, &alloc_info, &cmd);

  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin_info);
  return cmd;
}

void VkRenderer::EndSingleTimeCommands(VkCommandBuffer cmd) {
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &cmd;

  vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
  vkQueueWaitIdle(graphics_queue_);
  vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
}

void VkRenderer::TransitionImageLayout(VkCommandBuffer cmd,
                                       VkImage image,
                                       VkImageLayout old_layout,
                                       VkImageLayout new_layout,
                                       uint32_t src_queue_family,
                                       uint32_t dst_queue_family) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = src_queue_family;
  barrier.dstQueueFamilyIndex = dst_queue_family;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

  if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
      new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  }

  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VkRenderer::DestroySurfaceTexture(SurfaceTexture* tex) {
  if (!tex || !device_) return;
  vkDeviceWaitIdle(device_);
  if (tex->descriptor_set != VK_NULL_HANDLE) {
    vkFreeDescriptorSets(device_, descriptor_pool_, 1, &tex->descriptor_set);
    tex->descriptor_set = VK_NULL_HANDLE;
  }
  if (tex->view != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, tex->view, nullptr);
    tex->view = VK_NULL_HANDLE;
  }
  if (tex->image != VK_NULL_HANDLE) {
    vkDestroyImage(device_, tex->image, nullptr);
    tex->image = VK_NULL_HANDLE;
  }
  if (tex->memory != VK_NULL_HANDLE) {
    vkFreeMemory(device_, tex->memory, nullptr);
    tex->memory = VK_NULL_HANDLE;
  }
  if (tex->external_host_ptr != nullptr && tex->external_host_ptr != MAP_FAILED) {
    munmap(tex->external_host_ptr, tex->external_host_size);
    tex->external_host_ptr = nullptr;
    tex->external_host_size = 0;
  }
#ifdef __ANDROID__
  if (tex->persistent_ahb != nullptr) {
    if (tex->persistent_ahb_vaddr != nullptr) {
      AHardwareBuffer_unlock(tex->persistent_ahb, nullptr);
      tex->persistent_ahb_vaddr = nullptr;
    }
    AHardwareBuffer_release(tex->persistent_ahb);
    tex->persistent_ahb = nullptr;
    tex->persistent_ahb_stride = 0;
  }
#endif
}

bool VkRenderer::AllocateAndSetupTextureImage(SurfaceTexture* tex,
                                              uint32_t width,
                                              uint32_t height,
                                              VkFormat format) {
  if (tex->image != VK_NULL_HANDLE && !tex->is_external_dmabuf && tex->width == width &&
      tex->height == height && tex->format == format) {
    return true;
  }
  DestroySurfaceTexture(tex);

  tex->width = width;
  tex->height = height;
  tex->format = format;
  tex->is_external_dmabuf = false;

  VkImageCreateInfo img_info{};
  img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  img_info.imageType = VK_IMAGE_TYPE_2D;
  img_info.format = format;
  img_info.extent = {width, height, 1};
  img_info.mipLevels = 1;
  img_info.arrayLayers = 1;
  img_info.samples = VK_SAMPLE_COUNT_1_BIT;
  img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if (vkCreateImage(device_, &img_info, nullptr, &tex->image) != VK_SUCCESS) {
    LOGE(kTag, "vkCreateImage failed for surface texture (%ux%u)", width, height);
    return false;
  }

  VkMemoryRequirements reqs{};
  vkGetImageMemoryRequirements(device_, tex->image, &reqs);

  VkMemoryAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = reqs.size;
  alloc_info.memoryTypeIndex = FindMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  if (vkAllocateMemory(device_, &alloc_info, nullptr, &tex->memory) != VK_SUCCESS ||
      vkBindImageMemory(device_, tex->image, tex->memory, 0) != VK_SUCCESS) {
    LOGE(kTag, "Failed to allocate/bind surface texture memory");
    return false;
  }

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = tex->image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;

  if (vkCreateImageView(device_, &view_info, nullptr, &tex->view) != VK_SUCCESS) {
    LOGE(kTag, "vkCreateImageView failed for surface texture");
    return false;
  }

  VkDescriptorSetAllocateInfo ds_alloc{};
  ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ds_alloc.descriptorPool = descriptor_pool_;
  ds_alloc.descriptorSetCount = 1;
  ds_alloc.pSetLayouts = &descriptor_set_layout_;

  if (vkAllocateDescriptorSets(device_, &ds_alloc, &tex->descriptor_set) != VK_SUCCESS) {
    LOGE(kTag, "vkAllocateDescriptorSets failed");
    return false;
  }

  VkDescriptorImageInfo desc_img{};
  desc_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  desc_img.imageView = tex->view;
  desc_img.sampler = sampler_;

  VkWriteDescriptorSet write_ds{};
  write_ds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_ds.dstSet = tex->descriptor_set;
  write_ds.dstBinding = 0;
  write_ds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write_ds.descriptorCount = 1;
  write_ds.pImageInfo = &desc_img;

  vkUpdateDescriptorSets(device_, 1, &write_ds, 0, nullptr);
  return true;
}

bool VkRenderer::UploadShmBufferToTexture(const CommittedFrame& frame, SurfaceTexture* tex) {
  VkDeviceSize tight_row_bytes = static_cast<VkDeviceSize>(frame.width) * 4;
  VkDeviceSize required_size = tight_row_bytes * frame.height;
  if (required_size == 0 || frame.shm_pixels.size() < static_cast<size_t>(frame.stride) * frame.height) {
    LOGE(kTag, "Invalid wl_shm buffer size (%zu bytes for %ux%u stride %u)",
         frame.shm_pixels.size(), frame.width, frame.height, frame.stride);
    return false;
  }

  if (staging_capacity_ < required_size) {
    if (staging_buffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, staging_buffer_, nullptr);
      staging_buffer_ = VK_NULL_HANDLE;
    }
    if (staging_memory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, staging_memory_, nullptr);
      staging_memory_ = VK_NULL_HANDLE;
    }
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = required_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &buf_info, nullptr, &staging_buffer_) != VK_SUCCESS) {
      LOGE(kTag, "Failed to create staging VkBuffer");
      return false;
    }
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, staging_buffer_, &reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = reqs.size;
    alloc_info.memoryTypeIndex = FindMemoryType(
        reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &staging_memory_) != VK_SUCCESS ||
        vkBindBufferMemory(device_, staging_buffer_, staging_memory_, 0) != VK_SUCCESS) {
      LOGE(kTag, "Failed to allocate/bind staging VkDeviceMemory");
      return false;
    }
    staging_capacity_ = required_size;
  }

  void* mapped = nullptr;
  if (vkMapMemory(device_, staging_memory_, 0, required_size, 0, &mapped) != VK_SUCCESS) {
    LOGE(kTag, "vkMapMemory failed on staging buffer");
    return false;
  }
  uint8_t* dst = static_cast<uint8_t*>(mapped);
  const uint8_t* src = frame.shm_pixels.data();
  for (uint32_t row = 0; row < frame.height; ++row) {
    memcpy(dst + row * tight_row_bytes, src + row * frame.stride, tight_row_bytes);
    // If XRGB8888 (format == 1), ensure alpha byte is 0xFF so blending is opaque.
    if (frame.format == WL_SHM_FORMAT_XRGB8888) {
      uint8_t* row_ptr = dst + row * tight_row_bytes;
      for (uint32_t col = 0; col < frame.width; ++col) {
        row_ptr[col * 4 + 3] = 0xFF;
      }
    }
  }
  vkUnmapMemory(device_, staging_memory_);

  if (!AllocateAndSetupTextureImage(tex, frame.width, frame.height, VK_FORMAT_B8G8R8A8_UNORM)) {
    return false;
  }

  VkCommandBuffer cmd = BeginSingleTimeCommands();
  TransitionImageLayout(cmd, tex->image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {frame.width, frame.height, 1};

  vkCmdCopyBufferToImage(cmd, staging_buffer_, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &region);

  TransitionImageLayout(cmd, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EndSingleTimeCommands(cmd);

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.shm_uploads++;
  }
  LOGV(kTag, "Uploaded wl_shm buffer (%ux%u) to staging VkBuffer and blitted to VkImage",
       frame.width, frame.height);
  return true;
}

#ifdef __ANDROID__
struct NativeHandleHeader {
  int version;
  int numFds;
  int numInts;
  int data[0];
};

using PFN_AHardwareBuffer_createFromHandle = int (*)(const AHardwareBuffer_Desc* desc,
                                                     const void* handle,
                                                     int32_t method,
                                                     AHardwareBuffer** outBuffer);

PFN_AHardwareBuffer_createFromHandle GetAhbCreateFromHandleFn() {
  static PFN_AHardwareBuffer_createFromHandle fn = []() {
    auto p = reinterpret_cast<PFN_AHardwareBuffer_createFromHandle>(
        dlsym(RTLD_DEFAULT, "AHardwareBuffer_createFromHandle"));
    if (!p) {
      void* lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
      if (lib) {
        p = reinterpret_cast<PFN_AHardwareBuffer_createFromHandle>(
            dlsym(lib, "AHardwareBuffer_createFromHandle"));
      }
    }
    return p;
  }();
  return fn;
}

constexpr uint64_t kAhbHandlePlaneModifier = 0x414842484E444C00ULL;  // 'AHBHNDL\0'
constexpr uint32_t kAhbSerializedMagic = 0x41484248u;                 // 'AHBH'

struct AhbSerializedHeader {
  uint32_t magic = 0;
  AHardwareBuffer_Desc desc{};
  int32_t num_fds = 0;
  int32_t num_ints = 0;
};
#endif

bool VkRenderer::ImportDmaBufToTexture(const CommittedFrame& frame, SurfaceTexture* tex) {
  if (frame.dmabuf_fd < 0 && frame.shm_pixels.empty()) return false;

#ifdef __ANDROID__
  // Zero-copy Android GPU import via VK_ANDROID_external_memory_android_hardware_buffer
  // when a local Wayland client passes its AHardwareBuffer native_handle over zwp_linux_dmabuf_v1.
  if (has_android_ahb_ext_ && frame.planes.size() >= 2 &&
      frame.planes.back().modifier == kAhbHandlePlaneModifier && frame.planes.back().fd >= 0) {
    int meta_fd = frame.planes.back().fd;
    AhbSerializedHeader hdr{};
    if (pread(meta_fd, &hdr, sizeof(hdr), 0) == static_cast<ssize_t>(sizeof(hdr)) &&
        hdr.magic == kAhbSerializedMagic && hdr.num_fds >= 1 && hdr.num_fds <= 8 &&
        hdr.num_ints >= 0 && hdr.num_ints <= 128 &&
        frame.planes.size() >= static_cast<size_t>(hdr.num_fds + 1)) {
      const size_t handle_bytes =
          sizeof(NativeHandleHeader) +
          static_cast<size_t>(hdr.num_fds + hdr.num_ints) * sizeof(int);
      auto* nh = static_cast<NativeHandleHeader*>(std::calloc(1, handle_bytes));
      if (nh) {
        nh->version = sizeof(NativeHandleHeader);
        nh->numFds = hdr.num_fds;
        nh->numInts = hdr.num_ints;
        for (int i = 0; i < hdr.num_fds; ++i) {
          nh->data[i] = frame.planes[static_cast<size_t>(i)].fd;
        }
        const size_t ints_bytes = static_cast<size_t>(hdr.num_ints) * sizeof(int);
        auto create_fn = GetAhbCreateFromHandleFn();
        if (create_fn &&
            (ints_bytes == 0 ||
             pread(meta_fd, &nh->data[hdr.num_fds], ints_bytes, sizeof(hdr)) ==
                 static_cast<ssize_t>(ints_bytes))) {
          AHardwareBuffer* imported_ahb = nullptr;
          // Try method = 3 (CLONE) first so GraphicBuffer allocates its own native_handle_t copy.
          int res = create_fn(&hdr.desc, nh, 3, &imported_ahb);
          if (res == 0 && imported_ahb != nullptr) {
            std::free(nh);
            nh = nullptr;
          } else {
            // Fallback to method = 2 (REGISTER / TAKE_HANDLE): GraphicBuffer takes ownership of
            // the malloc'd `nh` and its FDs, and will free `nh` + close FDs when destroyed.
            for (int i = 0; i < hdr.num_fds; ++i) {
              nh->data[i] = dup(frame.planes[static_cast<size_t>(i)].fd);
            }
            res = create_fn(&hdr.desc, nh, 2, &imported_ahb);
            if (res == 0 && imported_ahb != nullptr) {
              nh = nullptr;  // Ownership transferred to imported_ahb
            } else {
              for (int i = 0; i < hdr.num_fds; ++i) {
                if (nh->data[i] >= 0) close(nh->data[i]);
              }
            }
          }
          if (nh) {
            std::free(nh);
            nh = nullptr;
          }
          if (imported_ahb != nullptr) {
            bool ok = ImportAndroidHardwareBuffer(frame.surface_id, imported_ahb, frame.width,
                                                  frame.height);
            AHardwareBuffer_release(imported_ahb);
            if (ok) {
              {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.dmabuf_imports++;
              }
              LOGI(kTag,
                   "Zero-copy imported Wayland zwp_linux_dmabuf_v1 AHardwareBuffer (fd=%d, %ux%u) "
                   "into VkImage via VK_ANDROID_external_memory_android_hardware_buffer",
                   frame.dmabuf_fd, frame.width, frame.height);
              return true;
            }
          }
        }
        if (nh) {
          std::free(nh);
        }
      }
    }
  }

  // Import standard Linux Wayland buffers (zwp_linux_dmabuf_v1 or local host buffers) into a
  // hardware GPU AHardwareBuffer bound to VkImage via VK_ANDROID_external_memory_android_hardware_buffer.
  if (has_android_ahb_ext_ && frame.width > 0 && frame.height > 0) {
    const uint8_t* src_base = nullptr;
    void* mapped_ptr = MAP_FAILED;
    size_t mapped_len = 0;
    const uint32_t plane_offset = frame.planes.empty() ? 0u : frame.planes[0].offset;
    const size_t plane_bytes = static_cast<size_t>(frame.stride) * frame.height;

    if (!frame.shm_pixels.empty() && frame.shm_pixels.size() >= plane_bytes) {
      src_base = frame.shm_pixels.data();
    } else if (frame.dmabuf_fd >= 0 && plane_bytes > 0) {
      mapped_len = static_cast<size_t>(plane_offset) + plane_bytes;
      mapped_ptr = mmap(nullptr, mapped_len, PROT_READ, MAP_SHARED, frame.dmabuf_fd, 0);
      if (mapped_ptr != MAP_FAILED) {
        src_base = static_cast<const uint8_t*>(mapped_ptr) + plane_offset;
      }
    }

    if (src_base != nullptr) {
      bool need_import = (tex->persistent_ahb == nullptr || tex->persistent_ahb_vaddr == nullptr ||
                          tex->image == VK_NULL_HANDLE || tex->width != frame.width ||
                          tex->height != frame.height);
      AHardwareBuffer* ahb = tex->persistent_ahb;
      void* dst_vaddr = tex->persistent_ahb_vaddr;
      uint32_t ahb_stride = tex->persistent_ahb_stride;

      if (need_import) {
        AHardwareBuffer_Desc ahb_desc{};
        ahb_desc.width = frame.width;
        ahb_desc.height = frame.height;
        ahb_desc.layers = 1;
        ahb_desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        ahb_desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                         AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                         AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;

        ahb = nullptr;
        dst_vaddr = nullptr;
        if (AHardwareBuffer_allocate(&ahb_desc, &ahb) == 0 && ahb != nullptr) {
          AHardwareBuffer_Desc actual_desc{};
          AHardwareBuffer_describe(ahb, &actual_desc);
          ahb_stride = actual_desc.stride > 0 ? actual_desc.stride : frame.width;
          if (AHardwareBuffer_lock(
                  ahb,
                  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                  -1, nullptr, &dst_vaddr) != 0 ||
              dst_vaddr == nullptr) {
            AHardwareBuffer_release(ahb);
            ahb = nullptr;
            dst_vaddr = nullptr;
          }
        }
      }

      if (ahb != nullptr && dst_vaddr != nullptr) {
        const uint32_t dst_stride_bytes =
            (ahb_stride > 0 ? ahb_stride : frame.width) * 4;
        auto* dst_bytes = static_cast<uint8_t*>(dst_vaddr);
        const bool is_rgba =
            (frame.format == 0x34324241u || frame.format == 0x34324258u);
        const bool force_opaque =
            (frame.format == WL_SHM_FORMAT_XRGB8888 || frame.format == 0x34325258u ||
             frame.format == 0x34324258u);
        const uint32_t alpha_or = force_opaque ? 0xFF000000u : 0u;

        for (uint32_t row = 0; row < frame.height; ++row) {
          const uint8_t* srow = src_base + static_cast<size_t>(row) * frame.stride;
          uint8_t* drow = dst_bytes + static_cast<size_t>(row) * dst_stride_bytes;
          if (is_rgba && !force_opaque) {
            std::memcpy(drow, srow, static_cast<size_t>(frame.width) * 4);
          } else if (is_rgba) {
            const auto* spx = reinterpret_cast<const uint32_t*>(srow);
            auto* dpx = reinterpret_cast<uint32_t*>(drow);
            for (uint32_t col = 0; col < frame.width; ++col) {
              dpx[col] = spx[col] | 0xFF000000u;
            }
          } else {
            const auto* spx = reinterpret_cast<const uint32_t*>(srow);
            auto* dpx = reinterpret_cast<uint32_t*>(drow);
            for (uint32_t col = 0; col < frame.width; ++col) {
              uint32_t v = spx[col];
              dpx[col] = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) |
                         ((v & 0x000000FFu) << 16) | alpha_or;
            }
          }
        }
        if (mapped_ptr != MAP_FAILED) {
          munmap(mapped_ptr, mapped_len);
          mapped_ptr = MAP_FAILED;
        }
        bool ok = true;
        if (need_import) {
          ok = ImportAndroidHardwareBuffer(frame.surface_id, ahb, frame.width, frame.height);
          if (ok) {
            tex->persistent_ahb = ahb;
            tex->persistent_ahb_vaddr = dst_vaddr;
            tex->persistent_ahb_stride = ahb_stride;
          } else {
            AHardwareBuffer_unlock(ahb, nullptr);
            AHardwareBuffer_release(ahb);
          }
        }
        if (ok) {
          uint64_t imports = 0;
          {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.dmabuf_imports++;
            imports = stats_.dmabuf_imports;
          }
          if (need_import || (imports % 60 == 1)) {
            LOGI(kTag,
                 "Imported Wayland buffer (%ux%u, format=0x%x) into GPU AHardwareBuffer via "
                 "VK_ANDROID_external_memory_android_hardware_buffer",
                 frame.width, frame.height, frame.format);
          }
          return true;
        }
      }
    }
    if (mapped_ptr != MAP_FAILED) {
      munmap(mapped_ptr, mapped_len);
    }
  }
#endif

  if (has_external_memory_fd_ || has_external_memory_host_) {
    DestroySurfaceTexture(tex);
    tex->width = frame.width;
    tex->height = frame.height;
    const bool is_rgba_fourcc =
        (frame.format == 0x34324241u || frame.format == 0x34324258u);
    const bool force_opaque =
        (frame.format == WL_SHM_FORMAT_XRGB8888 || frame.format == 0x34325258u ||
         frame.format == 0x34324258u);
    tex->format = is_rgba_fourcc ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
    tex->is_external_dmabuf = true;

    const uint32_t plane_offset = frame.planes.empty() ? 0u : frame.planes[0].offset;
    const bool tight_stride = (frame.stride == frame.width * 4 && plane_offset == 0);

    VkExternalMemoryHandleTypeFlagBits fd_handle_type =
        has_dma_buf_ext_ ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
                         : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    // Path A: Direct linear VkImage zero-copy import (when row pitch matches width * 4).
    if (tight_stride) {
      VkExternalMemoryImageCreateInfo ext_img_info{};
      ext_img_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
      ext_img_info.handleTypes =
          (has_external_memory_fd_ ? static_cast<VkExternalMemoryHandleTypeFlags>(fd_handle_type)
                                   : 0u) |
          (has_external_memory_host_
               ? static_cast<VkExternalMemoryHandleTypeFlags>(
                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT)
               : 0u);

      VkImageCreateInfo img_info{};
      img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      img_info.pNext = &ext_img_info;
      img_info.imageType = VK_IMAGE_TYPE_2D;
      img_info.format = tex->format;
      img_info.extent = {frame.width, frame.height, 1};
      img_info.mipLevels = 1;
      img_info.arrayLayers = 1;
      img_info.samples = VK_SAMPLE_COUNT_1_BIT;
      img_info.tiling = VK_IMAGE_TILING_LINEAR;
      img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      if (vkCreateImage(device_, &img_info, nullptr, &tex->image) == VK_SUCCESS) {
        VkImageSubresource subres{};
        subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        subres.mipLevel = 0;
        subres.arrayLayer = 0;
        VkSubresourceLayout subres_layout{};
        vkGetImageSubresourceLayout(device_, tex->image, &subres, &subres_layout);

        VkMemoryRequirements reqs{};
        vkGetImageMemoryRequirements(device_, tex->image, &reqs);

        bool bound_ok = false;
        const char* import_mechanism = "VK_KHR_external_memory_fd";

        if (subres_layout.rowPitch == frame.stride && has_external_memory_fd_) {
          uint32_t mem_type_bits = reqs.memoryTypeBits;
          auto fpGetMemoryFdPropertiesKHR = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
              vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
          if (fpGetMemoryFdPropertiesKHR &&
              fd_handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
            VkMemoryFdPropertiesKHR fd_props{};
            fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
            if (fpGetMemoryFdPropertiesKHR(device_, fd_handle_type, frame.dmabuf_fd, &fd_props) ==
                VK_SUCCESS) {
              mem_type_bits &= fd_props.memoryTypeBits;
            }
          }

          int imported_fd = dup(frame.dmabuf_fd);
          if (imported_fd >= 0) {
            VkMemoryDedicatedAllocateInfo dedicated_info{};
            dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicated_info.image = tex->image;

            VkImportMemoryFdInfoKHR import_info{};
            import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
            import_info.pNext = &dedicated_info;
            import_info.handleType = fd_handle_type;
            import_info.fd = imported_fd;

            VkMemoryAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc_info.pNext = &import_info;
            alloc_info.allocationSize = reqs.size;
            alloc_info.memoryTypeIndex = FindMemoryType(mem_type_bits, 0);

            VkResult alloc_res = vkAllocateMemory(device_, &alloc_info, nullptr, &tex->memory);
            if (alloc_res != VK_SUCCESS &&
                fd_handle_type != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
              import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
              alloc_res = vkAllocateMemory(device_, &alloc_info, nullptr, &tex->memory);
            }
            if (alloc_res == VK_SUCCESS &&
                vkBindImageMemory(device_, tex->image, tex->memory, 0) == VK_SUCCESS) {
              bound_ok = true;
            } else {
              if (alloc_res != VK_SUCCESS) {
                close(imported_fd);
              }
              if (tex->memory != VK_NULL_HANDLE) {
                vkFreeMemory(device_, tex->memory, nullptr);
                tex->memory = VK_NULL_HANDLE;
              }
            }
          }
        }

        // If client exported a read-only DRM PRIME DMA-BUF FD (e.g. Mesa gbm_bo_get_fd without
        // DRM_RDWR), map it PROT_READ and import zero-copy via VK_EXT_external_memory_host.
        if (!bound_ok && subres_layout.rowPitch == frame.stride && has_external_memory_host_) {
          size_t map_len =
              std::max<size_t>(reqs.size, static_cast<size_t>(frame.stride) * frame.height);
          void* host_ptr = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, frame.dmabuf_fd, 0);
          if (host_ptr != MAP_FAILED) {
            VkImportMemoryHostPointerInfoEXT host_import{};
            host_import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
            host_import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            host_import.pHostPointer = host_ptr;

            VkMemoryAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc_info.pNext = &host_import;
            alloc_info.allocationSize = reqs.size;
            alloc_info.memoryTypeIndex =
                FindMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

            if (vkAllocateMemory(device_, &alloc_info, nullptr, &tex->memory) == VK_SUCCESS &&
                vkBindImageMemory(device_, tex->image, tex->memory, 0) == VK_SUCCESS) {
              tex->external_host_ptr = host_ptr;
              tex->external_host_size = map_len;
              bound_ok = true;
              import_mechanism = "VK_EXT_external_memory_host[DMA-BUF]";
            } else {
              if (tex->memory != VK_NULL_HANDLE) {
                vkFreeMemory(device_, tex->memory, nullptr);
                tex->memory = VK_NULL_HANDLE;
              }
              munmap(host_ptr, map_len);
            }
          }
        }

        if (bound_ok) {
          VkImageViewCreateInfo view_info{};
          view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
          view_info.image = tex->image;
          view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
          view_info.format = tex->format;
          if (force_opaque) {
            view_info.components.a = VK_COMPONENT_SWIZZLE_ONE;
          }
          view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          view_info.subresourceRange.levelCount = 1;
          view_info.subresourceRange.layerCount = 1;

          if (vkCreateImageView(device_, &view_info, nullptr, &tex->view) == VK_SUCCESS) {
            VkDescriptorSetAllocateInfo ds_alloc{};
            ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ds_alloc.descriptorPool = descriptor_pool_;
            ds_alloc.descriptorSetCount = 1;
            ds_alloc.pSetLayouts = &descriptor_set_layout_;

            if (vkAllocateDescriptorSets(device_, &ds_alloc, &tex->descriptor_set) == VK_SUCCESS) {
              VkDescriptorImageInfo desc_img{};
              desc_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
              desc_img.imageView = tex->view;
              desc_img.sampler = sampler_;

              VkWriteDescriptorSet write_ds{};
              write_ds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
              write_ds.dstSet = tex->descriptor_set;
              write_ds.dstBinding = 0;
              write_ds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
              write_ds.descriptorCount = 1;
              write_ds.pImageInfo = &desc_img;
              vkUpdateDescriptorSets(device_, 1, &write_ds, 0, nullptr);

              VkCommandBuffer cmd = BeginSingleTimeCommands();
              TransitionImageLayout(cmd, tex->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
              EndSingleTimeCommands(cmd);

              {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.dmabuf_imports++;
              }
              LOGI(kTag,
                   "Zero-copy imported DMA-BUF fd=%d into VkImage (%ux%u, stride=%u) via %s",
                   frame.dmabuf_fd, frame.width, frame.height, frame.stride, import_mechanism);
              return true;
            }
          }
        }
      }
      DestroySurfaceTexture(tex);
    }

    // Path B: Padded DRM row stride (e.g. 64-pixel aligned dumb buffers where stride != width * 4).
    // Import the DMA-BUF directly into an external VkBuffer and GPU-blit with bufferRowLength = stride / 4.
    if (has_external_memory_host_ && frame.stride >= frame.width * 4 && (frame.stride % 4 == 0)) {
      const size_t total_plane_bytes =
          static_cast<size_t>(plane_offset) + static_cast<size_t>(frame.stride) * frame.height;
      const size_t aligned_bytes = (total_plane_bytes + 4095u) & ~static_cast<size_t>(4095u);
      void* host_ptr = mmap(nullptr, total_plane_bytes, PROT_READ, MAP_SHARED, frame.dmabuf_fd, 0);
      if (host_ptr != MAP_FAILED) {
        VkExternalMemoryBufferCreateInfo ext_buf_info{};
        ext_buf_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        ext_buf_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

        VkBufferCreateInfo buf_info{};
        buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_info.pNext = &ext_buf_info;
        buf_info.size = aligned_bytes;
        buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer ext_buf = VK_NULL_HANDLE;
        VkDeviceMemory ext_mem = VK_NULL_HANDLE;
        if (vkCreateBuffer(device_, &buf_info, nullptr, &ext_buf) == VK_SUCCESS) {
          VkMemoryRequirements buf_reqs{};
          vkGetBufferMemoryRequirements(device_, ext_buf, &buf_reqs);

          VkImportMemoryHostPointerInfoEXT host_import{};
          host_import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
          host_import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
          host_import.pHostPointer = host_ptr;

          VkMemoryAllocateInfo alloc_info{};
          alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
          alloc_info.pNext = &host_import;
          alloc_info.allocationSize = std::max<VkDeviceSize>(buf_reqs.size, aligned_bytes);
          alloc_info.memoryTypeIndex =
              FindMemoryType(buf_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

          if (vkAllocateMemory(device_, &alloc_info, nullptr, &ext_mem) == VK_SUCCESS &&
              vkBindBufferMemory(device_, ext_buf, ext_mem, 0) == VK_SUCCESS &&
              AllocateAndSetupTextureImage(tex, frame.width, frame.height, tex->format)) {
            if (force_opaque && tex->view != VK_NULL_HANDLE) {
              vkDestroyImageView(device_, tex->view, nullptr);
              tex->view = VK_NULL_HANDLE;
              VkImageViewCreateInfo view_info{};
              view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
              view_info.image = tex->image;
              view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
              view_info.format = tex->format;
              view_info.components.a = VK_COMPONENT_SWIZZLE_ONE;
              view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
              view_info.subresourceRange.levelCount = 1;
              view_info.subresourceRange.layerCount = 1;
              vkCreateImageView(device_, &view_info, nullptr, &tex->view);

              VkDescriptorImageInfo desc_img{};
              desc_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
              desc_img.imageView = tex->view;
              desc_img.sampler = sampler_;
              VkWriteDescriptorSet write_ds{};
              write_ds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
              write_ds.dstSet = tex->descriptor_set;
              write_ds.dstBinding = 0;
              write_ds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
              write_ds.descriptorCount = 1;
              write_ds.pImageInfo = &desc_img;
              vkUpdateDescriptorSets(device_, 1, &write_ds, 0, nullptr);
            }

            VkCommandBuffer cmd = BeginSingleTimeCommands();
            TransitionImageLayout(cmd, tex->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

            VkBufferImageCopy region{};
            region.bufferOffset = plane_offset;
            region.bufferRowLength = frame.stride / 4;
            region.bufferImageHeight = frame.height;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {frame.width, frame.height, 1};

            vkCmdCopyBufferToImage(cmd, ext_buf, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &region);

            TransitionImageLayout(cmd, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            EndSingleTimeCommands(cmd);

            vkDestroyBuffer(device_, ext_buf, nullptr);
            vkFreeMemory(device_, ext_mem, nullptr);
            munmap(host_ptr, total_plane_bytes);

            {
              std::lock_guard<std::mutex> lock(stats_mutex_);
              stats_.dmabuf_imports++;
            }
            LOGI(kTag,
                 "Imported padded DMA-BUF fd=%d (%ux%u, stride=%u) into VkImage via "
                 "VK_EXT_external_memory_host",
                 frame.dmabuf_fd, frame.width, frame.height, frame.stride);
            return true;
          }
          if (ext_buf != VK_NULL_HANDLE) vkDestroyBuffer(device_, ext_buf, nullptr);
          if (ext_mem != VK_NULL_HANDLE) vkFreeMemory(device_, ext_mem, nullptr);
        }
        munmap(host_ptr, total_plane_bytes);
      }
    }
  }

  // Fallback for non-DRM memfd descriptors on software ICDs: mmap the fd and upload via staging.
  size_t map_bytes = static_cast<size_t>(frame.stride) * frame.height;
  void* mapped = mmap(nullptr, map_bytes, PROT_READ, MAP_SHARED, frame.dmabuf_fd, 0);
  if (mapped != MAP_FAILED) {
    CommittedFrame temp = frame;
    temp.shm_pixels.assign(static_cast<const uint8_t*>(mapped),
                           static_cast<const uint8_t*>(mapped) + map_bytes);
    munmap(mapped, map_bytes);
    if (UploadShmBufferToTexture(temp, tex)) {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.dmabuf_imports++;
      LOGI(kTag, "Imported DMA-BUF fd=%d (%ux%u) into VkImage", frame.dmabuf_fd, frame.width,
           frame.height);
      return true;
    }
  }
  return false;
}

#ifdef __ANDROID__
bool VkRenderer::ImportAndroidHardwareBuffer(uint32_t surface_id,
                                             AHardwareBuffer* ahb,
                                             uint32_t width,
                                             uint32_t height) {
  if (!ahb || !has_android_ahb_ext_) return false;

  auto fpGetAhbProps = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
      vkGetDeviceProcAddr(device_, "vkGetAndroidHardwareBufferPropertiesANDROID"));
  if (!fpGetAhbProps) return false;

  VkAndroidHardwareBufferFormatPropertiesANDROID ahb_fmt_props{};
  ahb_fmt_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;

  VkAndroidHardwareBufferPropertiesANDROID ahb_props{};
  ahb_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
  ahb_props.pNext = &ahb_fmt_props;

  if (fpGetAhbProps(device_, ahb, &ahb_props) != VK_SUCCESS) {
    LOGE(kTag, "vkGetAndroidHardwareBufferPropertiesANDROID failed");
    return false;
  }

  SurfaceTexture& tex = surface_textures_[surface_id];
  uint32_t saved_parent = tex.parent_surface_id;
  std::string saved_app_id = tex.app_id;
  int32_t saved_x = tex.pos_x;
  int32_t saved_y = tex.pos_y;
  bool saved_popup = tex.is_popup;
  bool saved_sub = tex.is_subsurface;
  DestroySurfaceTexture(&tex);
  tex.surface_id = surface_id;
  tex.parent_surface_id = saved_parent;
  tex.app_id = std::move(saved_app_id);
  tex.pos_x = saved_x;
  tex.pos_y = saved_y;
  tex.is_popup = saved_popup;
  tex.is_subsurface = saved_sub;
  tex.width = width;
  tex.height = height;
  tex.format = (ahb_fmt_props.format != VK_FORMAT_UNDEFINED) ? ahb_fmt_props.format
                                                             : VK_FORMAT_R8G8B8A8_UNORM;
  tex.is_external_dmabuf = true;

  VkExternalMemoryImageCreateInfo ext_img_info{};
  ext_img_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext_img_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

  VkImageCreateInfo img_info{};
  img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  img_info.pNext = &ext_img_info;
  img_info.imageType = VK_IMAGE_TYPE_2D;
  img_info.format = tex.format;
  img_info.extent = {width, height, 1};
  img_info.mipLevels = 1;
  img_info.arrayLayers = 1;
  img_info.samples = VK_SAMPLE_COUNT_1_BIT;
  img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if (vkCreateImage(device_, &img_info, nullptr, &tex.image) != VK_SUCCESS) {
    return false;
  }

  VkMemoryDedicatedAllocateInfo dedicated_info{};
  dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated_info.image = tex.image;

  VkImportAndroidHardwareBufferInfoANDROID import_ahb{};
  import_ahb.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
  import_ahb.pNext = &dedicated_info;
  import_ahb.buffer = ahb;

  VkMemoryAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.pNext = &import_ahb;
  alloc_info.allocationSize = ahb_props.allocationSize;
  alloc_info.memoryTypeIndex = FindMemoryType(ahb_props.memoryTypeBits, 0);

  if (vkAllocateMemory(device_, &alloc_info, nullptr, &tex.memory) != VK_SUCCESS ||
      vkBindImageMemory(device_, tex.image, tex.memory, 0) != VK_SUCCESS) {
    DestroySurfaceTexture(&tex);
    return false;
  }

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = tex.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = tex.format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &view_info, nullptr, &tex.view) != VK_SUCCESS) {
    DestroySurfaceTexture(&tex);
    return false;
  }

  VkDescriptorSetAllocateInfo ds_alloc{};
  ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ds_alloc.descriptorPool = descriptor_pool_;
  ds_alloc.descriptorSetCount = 1;
  ds_alloc.pSetLayouts = &descriptor_set_layout_;
  vkAllocateDescriptorSets(device_, &ds_alloc, &tex.descriptor_set);

  VkDescriptorImageInfo desc_img{};
  desc_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  desc_img.imageView = tex.view;
  desc_img.sampler = sampler_;

  VkWriteDescriptorSet write_ds{};
  write_ds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_ds.dstSet = tex.descriptor_set;
  write_ds.dstBinding = 0;
  write_ds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write_ds.descriptorCount = 1;
  write_ds.pImageInfo = &desc_img;
  vkUpdateDescriptorSets(device_, 1, &write_ds, 0, nullptr);

  VkCommandBuffer cmd = BeginSingleTimeCommands();
  TransitionImageLayout(cmd, tex.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EndSingleTimeCommands(cmd);

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.ahb_imports++;
  }
  LOGI(kTag,
       "Imported AHardwareBuffer (%p, %ux%u) via "
       "VK_ANDROID_external_memory_android_hardware_buffer",
       static_cast<void*>(ahb), width, height);
  return true;
}
#endif

bool VkRenderer::RecordAndSubmitRenderPass(uint32_t image_index, const SurfaceTexture& active_tex) {
  vkWaitForFences(device_, 1, &in_flight_fence_, VK_TRUE, UINT64_MAX);
  vkResetFences(device_, 1, &in_flight_fence_);

  vkResetCommandBuffer(render_cmd_buffer_, 0);

  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(render_cmd_buffer_, &begin_info);

  VkClearValue clear_color = {{{0.08f, 0.09f, 0.12f, 1.0f}}};
  VkRenderPassBeginInfo rp_begin{};
  rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rp_begin.renderPass = render_pass_;
  rp_begin.framebuffer = framebuffers_[image_index];
  rp_begin.renderArea.offset = {0, 0};
  rp_begin.renderArea.extent = swapchain_extent_;
  rp_begin.clearValueCount = 1;
  rp_begin.pClearValues = &clear_color;

  vkCmdBeginRenderPass(render_cmd_buffer_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(render_cmd_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_);

  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = static_cast<float>(swapchain_extent_.width);
  viewport.height = static_cast<float>(swapchain_extent_.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(render_cmd_buffer_, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = swapchain_extent_;
  vkCmdSetScissor(render_cmd_buffer_, 0, 1, &scissor);

  VkBuffer vbufs[] = {vertex_buffer_};
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(render_cmd_buffer_, 0, 1, vbufs, offsets);

  // Pass 1: Draw base xdg_toplevel surfaces. Full-sized toplevel windows draw fullscreen first,
  // and smaller modal/dialog toplevel windows draw centered at their native size on top.
  std::string target_app_id;
  if (target_surface_id_ != 0) {
    auto tit = surface_textures_.find(target_surface_id_);
    if (tit != surface_textures_.end()) {
      target_app_id = tit->second.app_id;
    }
  }

  bool drew_base = false;
  std::vector<uint32_t> base_sids;
  for (const auto& [sid, surf_tex] : surface_textures_) {
    if (surf_tex.descriptor_set == VK_NULL_HANDLE) continue;
    if (surf_tex.is_popup || surf_tex.is_subsurface) continue;
    if (target_surface_id_ != 0 && sid != target_surface_id_) {
      const bool is_child_dialog =
          (surf_tex.parent_surface_id == target_surface_id_) ||
          (!target_app_id.empty() && surf_tex.app_id == target_app_id &&
           surf_tex.parent_surface_id == 0 &&
           (surf_tex.width < 640 || surf_tex.height < 400));
      if (!is_child_dialog) continue;
    }
    base_sids.push_back(sid);
  }
  std::sort(base_sids.begin(), base_sids.end(), [this](uint32_t a, uint32_t b) {
    if (target_surface_id_ != 0) {
      if (a == target_surface_id_ && b != target_surface_id_) return true;
      if (b == target_surface_id_ && a != target_surface_id_) return false;
    }
    const auto& ta = surface_textures_[a];
    const auto& tb = surface_textures_[b];
    const uint64_t area_a = static_cast<uint64_t>(ta.width) * ta.height;
    const uint64_t area_b = static_cast<uint64_t>(tb.width) * tb.height;
    if (area_a != area_b) return area_a > area_b;
    return a < b;
  });
  for (uint32_t sid : base_sids) {
    const auto& surf_tex = surface_textures_[sid];
    const bool is_dialog_size =
        (target_surface_id_ != 0 && sid != target_surface_id_) ||
        ((base_sids.size() > 1) && (surf_tex.width > 0 && surf_tex.height > 0) &&
         (surf_tex.parent_surface_id != 0 || surf_tex.width < 640 || surf_tex.height < 400));
    if (is_dialog_size) {
      int32_t dx = (static_cast<int32_t>(swapchain_extent_.width) -
                    static_cast<int32_t>(surf_tex.width)) /
                   2;
      int32_t dy = (static_cast<int32_t>(swapchain_extent_.height) -
                    static_cast<int32_t>(surf_tex.height)) /
                   2;
      if (dx < 0) dx = 0;
      if (dy < 0) dy = 0;
      VkViewport dlg_vp{};
      dlg_vp.x = static_cast<float>(dx);
      dlg_vp.y = static_cast<float>(dy);
      dlg_vp.width = static_cast<float>(surf_tex.width);
      dlg_vp.height = static_cast<float>(surf_tex.height);
      dlg_vp.minDepth = 0.0f;
      dlg_vp.maxDepth = 1.0f;
      VkRect2D dlg_sc{};
      dlg_sc.offset = {dx, dy};
      dlg_sc.extent = {std::min(surf_tex.width, swapchain_extent_.width - static_cast<uint32_t>(dx)),
                       std::min(surf_tex.height, swapchain_extent_.height - static_cast<uint32_t>(dy))};
      vkCmdSetViewport(render_cmd_buffer_, 0, 1, &dlg_vp);
      vkCmdSetScissor(render_cmd_buffer_, 0, 1, &dlg_sc);
    } else {
      vkCmdSetViewport(render_cmd_buffer_, 0, 1, &viewport);
      vkCmdSetScissor(render_cmd_buffer_, 0, 1, &scissor);
    }
    vkCmdBindDescriptorSets(render_cmd_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &surf_tex.descriptor_set, 0, nullptr);
    vkCmdDraw(render_cmd_buffer_, 6, 1, 0, 0);
    drew_base = true;
  }
  if (!drew_base && active_tex.descriptor_set != VK_NULL_HANDLE &&
      (target_surface_id_ == 0 || active_tex.surface_id == target_surface_id_)) {
    vkCmdSetViewport(render_cmd_buffer_, 0, 1, &viewport);
    vkCmdSetScissor(render_cmd_buffer_, 0, 1, &scissor);
    vkCmdBindDescriptorSets(render_cmd_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &active_tex.descriptor_set, 0, nullptr);
    vkCmdDraw(render_cmd_buffer_, 6, 1, 0, 0);
  }

  // Pass 2: Composite any active xdg_popup (menus, command palette, tooltips) or wl_subsurface
  // overlays on top at their exact (pos_x, pos_y, width, height) coordinates.
  for (const auto& [sid, surf_tex] : surface_textures_) {
    if (surf_tex.descriptor_set == VK_NULL_HANDLE) continue;
    if (!surf_tex.is_popup && !surf_tex.is_subsurface) continue;
    if (target_surface_id_ != 0 && surf_tex.parent_surface_id != 0 &&
        surf_tex.parent_surface_id != target_surface_id_) {
      continue;
    }

    VkViewport overlay_vp{};
    overlay_vp.x = static_cast<float>(surf_tex.pos_x);
    overlay_vp.y = static_cast<float>(surf_tex.pos_y);
    overlay_vp.width = static_cast<float>(std::max(surf_tex.width, 1u));
    overlay_vp.height = static_cast<float>(std::max(surf_tex.height, 1u));
    overlay_vp.minDepth = 0.0f;
    overlay_vp.maxDepth = 1.0f;
    vkCmdSetViewport(render_cmd_buffer_, 0, 1, &overlay_vp);
    vkCmdSetScissor(render_cmd_buffer_, 0, 1, &scissor);

    vkCmdBindDescriptorSets(render_cmd_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &surf_tex.descriptor_set, 0, nullptr);
    vkCmdDraw(render_cmd_buffer_, 6, 1, 0, 0);
  }

  vkCmdEndRenderPass(render_cmd_buffer_);

  bool should_sample_readback = !use_swapchain_;
#ifdef __ANDROID__
  if (use_swapchain_) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    uint64_t next_frame = stats_.frames_rendered + 1;
    should_sample_readback = (next_frame <= 5) || (next_frame % 60 == 0);
  }
#endif

  // Copy center pixel to readback_buffer_ to verify non-zero GPU composition output.
  if (readback_buffer_ != VK_NULL_HANDLE && should_sample_readback) {
    VkImage src_img = swapchain_images_[image_index];
    VkImageLayout cur_layout =
        use_swapchain_ ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    if (use_swapchain_) {
      VkImageMemoryBarrier to_src{};
      to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_src.image = src_img;
      to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      to_src.subresourceRange.levelCount = 1;
      to_src.subresourceRange.layerCount = 1;
      to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(render_cmd_buffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
      cur_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    VkBufferImageCopy copy_region{};
    copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.imageSubresource.layerCount = 1;
#ifdef __ANDROID__
    const bool copy_single_center_pixel = use_swapchain_;
#else
    const bool copy_single_center_pixel = false;
#endif
    if (copy_single_center_pixel) {
      copy_region.imageOffset = {static_cast<int32_t>(swapchain_extent_.width / 2),
                                 static_cast<int32_t>(swapchain_extent_.height / 2), 0};
      copy_region.imageExtent = {1, 1, 1};
    } else {
      copy_region.imageOffset = {0, 0, 0};
      copy_region.imageExtent = {swapchain_extent_.width, swapchain_extent_.height, 1};
    }
    vkCmdCopyImageToBuffer(render_cmd_buffer_, src_img, cur_layout, readback_buffer_, 1,
                           &copy_region);

    if (use_swapchain_) {
      VkImageMemoryBarrier to_present{};
      to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_present.image = src_img;
      to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      to_present.subresourceRange.levelCount = 1;
      to_present.subresourceRange.layerCount = 1;
      to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      to_present.dstAccessMask = 0;
      vkCmdPipelineBarrier(render_cmd_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &to_present);
    }
  }

  vkEndCommandBuffer(render_cmd_buffer_);

  VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  if (use_swapchain_) {
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_available_sem_;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_finished_sem_;
  }
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &render_cmd_buffer_;

  if (vkQueueSubmit(graphics_queue_, 1, &submit_info, in_flight_fence_) != VK_SUCCESS) {
    LOGE(kTag, "vkQueueSubmit failed");
    return false;
  }

  if (readback_memory_ != VK_NULL_HANDLE && should_sample_readback) {
    vkWaitForFences(device_, 1, &in_flight_fence_, VK_TRUE, UINT64_MAX);
    uint32_t pixel = 0;
    void* ptr = nullptr;
#ifdef __ANDROID__
    const bool copy_single_center_pixel = use_swapchain_;
#else
    const bool copy_single_center_pixel = false;
#endif
    VkDeviceSize map_bytes =
        copy_single_center_pixel
            ? sizeof(uint32_t)
            : static_cast<VkDeviceSize>(swapchain_extent_.width) * swapchain_extent_.height * 4;
    if (vkMapMemory(device_, readback_memory_, 0, map_bytes, 0, &ptr) == VK_SUCCESS) {
      const auto* pixels = static_cast<const uint32_t*>(ptr);
      size_t center_idx =
          copy_single_center_pixel
              ? 0u
              : static_cast<size_t>(swapchain_extent_.height / 2) * swapchain_extent_.width +
                    (swapchain_extent_.width / 2);
      pixel = pixels[center_idx];
      vkUnmapMemory(device_, readback_memory_);
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.last_sampled_pixel_rgba = pixel;
    }
  }

  if (use_swapchain_) {
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished_sem_;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &image_index;

    VkResult pres = vkQueuePresentKHR(present_queue_, &present_info);
    if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR) {
      LOGE(kTag, "vkQueuePresentKHR failed: %d", pres);
      return false;
    }
  }

  uint32_t sampled = 0;
  uint64_t rendered_count = 0;
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.frames_rendered++;
    stats_.presents_succeeded++;
    rendered_count = stats_.frames_rendered;
    sampled = stats_.last_sampled_pixel_rgba;
  }

  if (rendered_count <= 5 || (rendered_count % 60 == 0)) {
    LOGI(kTag,
         "Presented frame #%llu via %s (swapchainImage=%u, surface=%u, centerPixel=0x%08x)",
         static_cast<unsigned long long>(rendered_count),
         use_swapchain_ ? "vkQueuePresentKHR" : "vkQueueSubmit[Offscreen]", image_index,
         active_tex.surface_id, sampled);
  }
  return true;
}

bool VkRenderer::ProcessAndPresentFrame(const CommittedFrame& frame) {
  std::lock_guard<std::mutex> render_lock(render_mutex_);
  if (!initialized_) {
    if (frame.dmabuf_fd >= 0) close(frame.dmabuf_fd);
    for (const auto& p : frame.planes) {
      if (p.fd >= 0) close(p.fd);
    }
    return false;
  }

  if (frame.is_unmapped) {
    if (frame.dmabuf_fd >= 0) close(frame.dmabuf_fd);
    for (const auto& p : frame.planes) {
      if (p.fd >= 0) close(p.fd);
    }
    auto it = surface_textures_.find(frame.surface_id);
    if (it != surface_textures_.end()) {
      DestroySurfaceTexture(&it->second);
      surface_textures_.erase(it);
      LOGI(kTag, "Removed unmapped/destroyed surface %u from Vulkan compositor", frame.surface_id);
    }
    if (surface_textures_.empty()) {
      return true;
    }
    uint32_t image_index = 0;
    if (use_swapchain_) {
      VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_sem_,
                                           VK_NULL_HANDLE, &image_index);
      if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        LOGE(kTag, "vkAcquireNextImageKHR failed: %d", acq);
        return false;
      }
    }
    SurfaceTexture dummy{};
    return RecordAndSubmitRenderPass(image_index, dummy);
  }

  SurfaceTexture& tex = surface_textures_[frame.surface_id];
  tex.surface_id = frame.surface_id;
  tex.parent_surface_id = frame.parent_surface_id;
  if (!frame.app_id.empty()) {
    tex.app_id = frame.app_id;
  }
  tex.pos_x = frame.x;
  tex.pos_y = frame.y;
  tex.is_popup = frame.is_popup;
  tex.is_subsurface = frame.is_subsurface;

  bool ok = false;
  if (frame.buffer_type == BufferType::kShm) {
    ok = UploadShmBufferToTexture(frame, &tex);
  } else if (frame.buffer_type == BufferType::kDmaBuf) {
    ok = ImportDmaBufToTexture(frame, &tex);
  }

  // Close duplicated DMA-BUF descriptors now that Vulkan import is complete.
  if (frame.dmabuf_fd >= 0) {
    close(frame.dmabuf_fd);
  }
  for (const auto& p : frame.planes) {
    if (p.fd >= 0) close(p.fd);
  }

  if (!ok) return false;

  uint32_t image_index = 0;
  if (use_swapchain_) {
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_sem_,
                                         VK_NULL_HANDLE, &image_index);
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
      LOGE(kTag, "vkAcquireNextImageKHR failed: %d", acq);
      return false;
    }
  }

  return RecordAndSubmitRenderPass(image_index, tex);
}

bool VkRenderer::Resize(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) return false;
  std::lock_guard<std::mutex> render_lock(render_mutex_);
  if (config_.width == width && config_.height == height &&
      swapchain_extent_.width == width && swapchain_extent_.height == height) {
    return true;
  }
  config_.width = width;
  config_.height = height;
  swapchain_extent_ = {width, height};
  if (!initialized_ || !device_) return true;

  vkDeviceWaitIdle(device_);

  for (VkFramebuffer fb : framebuffers_) {
    if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, fb, nullptr);
  }
  framebuffers_.clear();

  for (VkImageView view : swapchain_image_views_) {
    if (view != VK_NULL_HANDLE) vkDestroyImageView(device_, view, nullptr);
  }
  swapchain_image_views_.clear();
  swapchain_images_.clear();

  if (swapchain_ != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
  if (offscreen_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, offscreen_image_, nullptr);
    offscreen_image_ = VK_NULL_HANDLE;
  }
  if (offscreen_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, offscreen_memory_, nullptr);
    offscreen_memory_ = VK_NULL_HANDLE;
  }
  offscreen_view_ = VK_NULL_HANDLE;

  if (readback_buffer_ != VK_NULL_HANDLE) {
    vkDestroyBuffer(device_, readback_buffer_, nullptr);
    readback_buffer_ = VK_NULL_HANDLE;
  }
  if (readback_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, readback_memory_, nullptr);
    readback_memory_ = VK_NULL_HANDLE;
  }

  if (!CreateSwapchainOrOffscreenTarget() || !CreateFramebuffers()) {
    LOGE(kTag, "Failed to recreate swapchain/framebuffers on Resize(%ux%u)", width, height);
    return false;
  }

  if (!surface_textures_.empty()) {
    uint32_t image_index = 0;
    if (use_swapchain_) {
      VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_sem_,
                                           VK_NULL_HANDLE, &image_index);
      if (acq == VK_SUCCESS || acq == VK_SUBOPTIMAL_KHR) {
        SurfaceTexture dummy{};
        RecordAndSubmitRenderPass(image_index, dummy);
      }
    } else {
      SurfaceTexture dummy{};
      RecordAndSubmitRenderPass(0, dummy);
    }
  }
  LOGI(kTag, "Resized VkRenderer presentation target to %ux%u", swapchain_extent_.width,
       swapchain_extent_.height);
  return true;
}

void VkRenderer::SetTargetSurfaceId(uint32_t surface_id) {
  std::lock_guard<std::mutex> render_lock(render_mutex_);
  target_surface_id_ = surface_id;
}

uint32_t VkRenderer::TargetSurfaceId() const {
  std::lock_guard<std::mutex> render_lock(render_mutex_);
  return target_surface_id_;
}

bool VkRenderer::Repaint() {
  std::lock_guard<std::mutex> render_lock(render_mutex_);
  if (!initialized_ || !device_ || surface_textures_.empty()) return false;
  uint32_t image_index = 0;
  if (use_swapchain_) {
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_sem_,
                                         VK_NULL_HANDLE, &image_index);
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
      return false;
    }
  }
  SurfaceTexture dummy{};
  return RecordAndSubmitRenderPass(image_index, dummy);
}

VkRenderer::RenderStats VkRenderer::GetStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

bool VkRenderer::SaveLastFramePpm(const std::string& path) {
  std::lock_guard<std::mutex> render_lock(render_mutex_);
  if (!initialized_ || readback_memory_ == VK_NULL_HANDLE) return false;
  const uint32_t w = swapchain_extent_.width;
  const uint32_t h = swapchain_extent_.height;
  VkDeviceSize total_bytes = static_cast<VkDeviceSize>(w) * h * 4;
  void* ptr = nullptr;
  if (vkMapMemory(device_, readback_memory_, 0, total_bytes, 0, &ptr) != VK_SUCCESS) {
    return false;
  }
  FILE* fp = fopen(path.c_str(), "wb");
  if (!fp) {
    vkUnmapMemory(device_, readback_memory_);
    return false;
  }
  fprintf(fp, "P6\n%u %u\n255\n", w, h);
  const auto* bgra = static_cast<const uint8_t*>(ptr);
  std::vector<uint8_t> rgb_row(w * 3);
  const bool is_bgra = (swapchain_format_ == VK_FORMAT_B8G8R8A8_UNORM ||
                        swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x) {
      if (is_bgra) {
        rgb_row[x * 3 + 0] = row[x * 4 + 2];
        rgb_row[x * 3 + 1] = row[x * 4 + 1];
        rgb_row[x * 3 + 2] = row[x * 4 + 0];
      } else {
        rgb_row[x * 3 + 0] = row[x * 4 + 0];
        rgb_row[x * 3 + 1] = row[x * 4 + 1];
        rgb_row[x * 3 + 2] = row[x * 4 + 2];
      }
    }
    fwrite(rgb_row.data(), 1, rgb_row.size(), fp);
  }
  fclose(fp);
  vkUnmapMemory(device_, readback_memory_);
  LOGI(kTag, "Saved rendered Vulkan swapchain frame (%ux%u) to '%s'", w, h, path.c_str());
  return true;
}

void VkRenderer::Shutdown() {
  if (!initialized_) return;
  vkDeviceWaitIdle(device_);

  for (auto& kv : surface_textures_) {
    DestroySurfaceTexture(&kv.second);
  }
  surface_textures_.clear();

  if (staging_buffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, staging_buffer_, nullptr);
  if (staging_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, staging_memory_, nullptr);
  if (vertex_buffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
  if (vertex_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, vertex_memory_, nullptr);
  if (readback_buffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, readback_buffer_, nullptr);
  if (readback_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, readback_memory_, nullptr);

  if (image_available_sem_ != VK_NULL_HANDLE)
    vkDestroySemaphore(device_, image_available_sem_, nullptr);
  if (render_finished_sem_ != VK_NULL_HANDLE)
    vkDestroySemaphore(device_, render_finished_sem_, nullptr);
  if (in_flight_fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, in_flight_fence_, nullptr);
  if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);

  for (VkFramebuffer fb : framebuffers_) {
    if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, fb, nullptr);
  }
  framebuffers_.clear();

  if (graphics_pipeline_ != VK_NULL_HANDLE)
    vkDestroyPipeline(device_, graphics_pipeline_, nullptr);
  if (pipeline_layout_ != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  if (descriptor_pool_ != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
  if (descriptor_set_layout_ != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
  if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
  if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, render_pass_, nullptr);

  for (VkImageView view : swapchain_image_views_) {
    if (view != VK_NULL_HANDLE) vkDestroyImageView(device_, view, nullptr);
  }
  swapchain_image_views_.clear();

  if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  if (offscreen_image_ != VK_NULL_HANDLE) vkDestroyImage(device_, offscreen_image_, nullptr);
  if (offscreen_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, offscreen_memory_, nullptr);

  if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
  if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);

  initialized_ = false;
}

}  // namespace vkwlf
