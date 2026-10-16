#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#endif

#include <vulkan/vulkan.h>
#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#endif

#include "wayland_server_engine.h"

#ifndef __ANDROID__
struct ANativeWindow;
struct AHardwareBuffer;
#endif

namespace vkwlf {

class VkRenderer {
 public:
  struct Config {
    uint32_t width = 1280;
    uint32_t height = 720;
    bool enable_validation = false;
  };

  struct RenderStats {
    uint64_t frames_rendered = 0;
    uint64_t shm_uploads = 0;
    uint64_t dmabuf_imports = 0;
    uint64_t ahb_imports = 0;
    uint64_t presents_succeeded = 0;
    uint32_t last_sampled_pixel_rgba = 0;
  };

  explicit VkRenderer(const Config& config);
  ~VkRenderer();

  VkRenderer(const VkRenderer&) = delete;
  VkRenderer& operator=(const VkRenderer&) = delete;

  // Initializes Vulkan instance, physical/logical device, swapchain (or headless/offscreen
  // presentation target), command pools, descriptor sets, and textured quad graphics pipeline.
  bool Initialize(ANativeWindow* native_window = nullptr);

  // Handles ANativeWindow resize / recreation.
  bool Resize(uint32_t width, uint32_t height);

  // Processes a committed Wayland surface frame:
  // - If wl_shm: uploads to staging VkBuffer and blits/copies into a sampled VkImage.
  // - If dma_buf: imports the DMA-BUF fd into a VkImage via VK_KHR_external_memory_fd
  //   (or VK_ANDROID_external_memory_android_hardware_buffer when backed by AHardwareBuffer).
  // - Executes the Vulkan render pass drawing the positioned textured quad for active
  //   xdg_toplevel surfaces and presents via vkQueuePresentKHR.
  bool ProcessAndPresentFrame(const CommittedFrame& frame);

#ifdef __ANDROID__
  // Direct zero-copy import of an Android AHardwareBuffer into a surface texture.
  bool ImportAndroidHardwareBuffer(uint32_t surface_id,
                                   AHardwareBuffer* ahb,
                                   uint32_t width,
                                   uint32_t height);
#endif

  void Shutdown();

  // Saves the most recently rendered Vulkan swapchain/offscreen frame to a binary PPM (P6) file.
  bool SaveLastFramePpm(const std::string& path);

  RenderStats GetStats() const;

 private:
  struct SurfaceTexture {
    uint32_t surface_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    int32_t pos_x = 0;
    int32_t pos_y = 0;
    bool is_popup = false;
    bool is_subsurface = false;
    bool is_external_dmabuf = false;
    AHardwareBuffer* persistent_ahb = nullptr;
    void* persistent_ahb_vaddr = nullptr;
    uint32_t persistent_ahb_stride = 0;
  };

  struct Vertex {
    float pos[2];
    float uv[2];
  };

  bool CreateInstance(bool has_android_window);
  bool PickPhysicalDevice();
  bool CreateLogicalDevice();
  bool CreateSurface(ANativeWindow* native_window);
  bool CreateSwapchainOrOffscreenTarget();
  bool CreateRenderPass();
  bool CreateDescriptorResources();
  bool CreateGraphicsPipeline();
  bool CreateFramebuffers();
  bool CreateCommandResources();
  bool CreateVertexBuffer();

  bool UploadShmBufferToTexture(const CommittedFrame& frame, SurfaceTexture* tex);
  bool ImportDmaBufToTexture(const CommittedFrame& frame, SurfaceTexture* tex);
  bool AllocateAndSetupTextureImage(SurfaceTexture* tex,
                                    uint32_t width,
                                    uint32_t height,
                                    VkFormat format);
  void DestroySurfaceTexture(SurfaceTexture* tex);

  bool UpdateQuadVertices(int32_t x, int32_t y, uint32_t w, uint32_t h);
  bool RecordAndSubmitRenderPass(uint32_t image_index, const SurfaceTexture& active_tex);

  uint32_t FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) const;
  VkCommandBuffer BeginSingleTimeCommands();
  void EndSingleTimeCommands(VkCommandBuffer cmd);
  void TransitionImageLayout(VkCommandBuffer cmd,
                             VkImage image,
                             VkImageLayout old_layout,
                             VkImageLayout new_layout,
                             uint32_t src_queue_family = VK_QUEUE_FAMILY_IGNORED,
                             uint32_t dst_queue_family = VK_QUEUE_FAMILY_IGNORED);

  Config config_;
  bool initialized_ = false;
  bool use_swapchain_ = false;
  bool has_external_memory_fd_ = false;
  bool has_dma_buf_ext_ = false;
  bool has_android_ahb_ext_ = false;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t graphics_queue_family_ = 0;
  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  VkQueue present_queue_ = VK_NULL_HANDLE;

  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
  VkExtent2D swapchain_extent_{1280, 720};
  std::vector<VkImage> swapchain_images_;
  std::vector<VkImageView> swapchain_image_views_;
  std::vector<VkFramebuffer> framebuffers_;

  // Used when running without a WSI surface (e.g. headless verification fallback)
  VkImage offscreen_image_ = VK_NULL_HANDLE;
  VkDeviceMemory offscreen_memory_ = VK_NULL_HANDLE;
  VkImageView offscreen_view_ = VK_NULL_HANDLE;
  VkBuffer readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;

  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline graphics_pipeline_ = VK_NULL_HANDLE;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer render_cmd_buffer_ = VK_NULL_HANDLE;
  VkSemaphore image_available_sem_ = VK_NULL_HANDLE;
  VkSemaphore render_finished_sem_ = VK_NULL_HANDLE;
  VkFence in_flight_fence_ = VK_NULL_HANDLE;

  // Staging buffer for wl_shm uploads
  VkBuffer staging_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
  VkDeviceSize staging_capacity_ = 0;

  // Vertex buffer for positioned xdg_toplevel quad
  VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;

  std::unordered_map<uint32_t, SurfaceTexture> surface_textures_;
  mutable std::mutex render_mutex_;
  mutable std::mutex stats_mutex_;
  RenderStats stats_{};
};

}  // namespace vkwlf
