/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_VULKAN_VULKAN_SWAP_CHAIN_H_
#define XENIA_UI_VULKAN_VULKAN_SWAP_CHAIN_H_

#include <memory>
#include <string>
#include <vector>

#include "xenia/ui/vulkan/vulkan.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace ui {
namespace vulkan {

class VulkanDevice;
class VulkanInstance;

class VulkanSwapChain {
 public:
  VulkanSwapChain(VulkanInstance* instance, VulkanDevice* device);
  ~VulkanSwapChain();

  VkSwapchainKHR handle = nullptr;

  operator VkSwapchainKHR() const { return handle; }

  uint32_t surface_width() const { return surface_width_; }
  uint32_t surface_height() const { return surface_height_; }
  VkImage surface_image() const {
    return buffers_[current_buffer_index_].image;
  }

  // Render pass used for compositing.
  VkRenderPass render_pass() const { return render_pass_; }
  // Render command buffer, active inside the render pass from Begin to End.
  VkCommandBuffer render_cmd_buffer() const { return render_cmd_buffer_; }
  // Copy commands, ran before the render command buffer.
  VkCommandBuffer copy_cmd_buffer() const { return copy_cmd_buffer_; }

  // Initializes the swap chain with the given WSI surface.
  bool Initialize(VkSurfaceKHR surface);
  // Reinitializes the swap chain with the initial surface.
  // The surface will be retained but all other swap chain resources will be
  // torn down and recreated with the new surface properties (size/etc).
  bool Reinitialize();

  // Begins the swap operation, preparing state for rendering.
  bool Begin();
  // Ends the swap operation, finalizing rendering and presenting the results.
  bool End();

 private:
  struct Buffer {
    VkImage image = nullptr;
    VkImageView image_view = nullptr;
    VkFramebuffer framebuffer = nullptr;
  };

  bool InitializeBuffer(Buffer* buffer, VkImage target_image);
  void DestroyBuffer(Buffer* buffer);

  // Safely releases all swap chain resources.
  void Shutdown();

  VulkanInstance* instance_ = nullptr;
  VulkanDevice* device_ = nullptr;

  VkSurfaceKHR surface_ = nullptr;
  uint32_t surface_width_ = 0;
  uint32_t surface_height_ = 0;
  VkFormat surface_format_ = VK_FORMAT_UNDEFINED;
  VkCommandPool cmd_pool_ = nullptr;
  VkCommandBuffer copy_cmd_buffer_ = nullptr;
  VkCommandBuffer render_cmd_buffer_ = nullptr;
  VkRenderPass render_pass_ = nullptr;
  VkSemaphore image_available_semaphore_ = nullptr;
  uint32_t current_buffer_index_ = 0;
  std::vector<Buffer> buffers_;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_VULKAN_SWAP_CHAIN_H_
