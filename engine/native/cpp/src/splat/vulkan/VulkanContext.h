// A small Vulkan compute harness: just enough to run the splat shaders.
//
// Vulkan is loaded at runtime with dlopen rather than linked, because the app
// supports devices that have no Vulkan driver at all and a missing symbol at
// load time would take the whole process with it. Everything here is compute
// only -- no surfaces, no swapchain, no graphics pipeline.
#pragma once

#if defined(__ANDROID__)

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace parallax {
namespace vk {

/// Every entry point the splat pipeline uses, in one list so the loader and
/// the declarations cannot drift apart.
#define PARALLAX_VK_GLOBAL_FUNCTIONS(X) \
  X(vkCreateInstance)                   \
  X(vkEnumerateInstanceVersion)

#define PARALLAX_VK_INSTANCE_FUNCTIONS(X)   \
  X(vkDestroyInstance)                      \
  X(vkEnumeratePhysicalDevices)             \
  X(vkGetPhysicalDeviceProperties)          \
  X(vkGetPhysicalDeviceMemoryProperties)    \
  X(vkGetPhysicalDeviceQueueFamilyProperties) \
  X(vkCreateDevice)                         \
  X(vkGetDeviceProcAddr)

#define PARALLAX_VK_DEVICE_FUNCTIONS(X) \
  X(vkDestroyDevice)                    \
  X(vkDeviceWaitIdle)                   \
  X(vkGetDeviceQueue)                   \
  X(vkCreateBuffer)                     \
  X(vkDestroyBuffer)                    \
  X(vkGetBufferMemoryRequirements)      \
  X(vkAllocateMemory)                   \
  X(vkFreeMemory)                       \
  X(vkBindBufferMemory)                 \
  X(vkMapMemory)                        \
  X(vkUnmapMemory)                      \
  X(vkFlushMappedMemoryRanges)          \
  X(vkInvalidateMappedMemoryRanges)     \
  X(vkCreateShaderModule)               \
  X(vkDestroyShaderModule)              \
  X(vkCreateDescriptorSetLayout)        \
  X(vkDestroyDescriptorSetLayout)       \
  X(vkCreatePipelineLayout)             \
  X(vkDestroyPipelineLayout)            \
  X(vkCreateComputePipelines)           \
  X(vkDestroyPipeline)                  \
  X(vkCreateDescriptorPool)             \
  X(vkDestroyDescriptorPool)            \
  X(vkAllocateDescriptorSets)           \
  X(vkUpdateDescriptorSets)             \
  X(vkCreateCommandPool)                \
  X(vkDestroyCommandPool)               \
  X(vkAllocateCommandBuffers)           \
  X(vkBeginCommandBuffer)               \
  X(vkEndCommandBuffer)                 \
  X(vkResetCommandBuffer)               \
  X(vkCmdBindPipeline)                  \
  X(vkCmdBindDescriptorSets)            \
  X(vkCmdDispatch)                      \
  X(vkCmdPipelineBarrier)               \
  X(vkCmdPushConstants)                 \
  X(vkCmdFillBuffer)                    \
  X(vkCmdCopyBuffer)                    \
  X(vkQueueSubmit)                      \
  X(vkCreateFence)                      \
  X(vkDestroyFence)                     \
  X(vkWaitForFences)                    \
  X(vkResetFences)

class Context;

/// A device-local or host-visible buffer, with its memory.
class Buffer {
 public:
  Buffer() = default;
  ~Buffer();
  Buffer(Buffer&& other) noexcept { *this = std::move(other); }
  Buffer& operator=(Buffer&& other) noexcept;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  bool valid() const { return handle_ != VK_NULL_HANDLE; }
  VkBuffer handle() const { return handle_; }
  VkDeviceSize size() const { return size_; }
  /// Non-null only for host-visible buffers.
  void* mapped() const { return mapped_; }

 private:
  friend class Context;
  const Context* context_ = nullptr;
  VkBuffer handle_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  VkDeviceSize size_ = 0;
  void* mapped_ = nullptr;
  bool coherent_ = false;
};

/// A compute pipeline together with the descriptor set it is driven through.
/// One set per pipeline: the bindings never change between dispatches, so
/// there is nothing to gain from sharing.
struct Pipeline {
  VkShaderModule module = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  uint32_t push_constant_size = 0;
};

/// One binding in a pipeline's descriptor set.
struct Binding {
  uint32_t index = 0;
  VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  const Buffer* buffer = nullptr;
};

class Context {
 public:
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  /// Returns null and sets `error` when Vulkan is missing, too old, or has no
  /// compute queue -- all of which are normal on older hardware.
  static std::unique_ptr<Context> create(std::string* error);

  const std::string& deviceName() const { return device_name_; }
  uint32_t maxWorkgroupInvocations() const { return max_workgroup_invocations_; }
  VkDeviceSize maxStorageBufferRange() const { return max_storage_buffer_range_; }

  Buffer createBuffer(VkDeviceSize size, bool host_visible, std::string* error) const;
  bool upload(const Buffer& buffer, const void* data, VkDeviceSize size,
              VkDeviceSize offset = 0) const;
  bool download(const Buffer& buffer, void* data, VkDeviceSize size,
                VkDeviceSize offset = 0) const;

  bool createPipeline(const uint32_t* spirv, size_t word_count, uint32_t push_constant_size,
                      const std::vector<Binding>& bindings, Pipeline* out,
                      std::string* error) const;
  void destroyPipeline(Pipeline* pipeline) const;

  // --- recording -----------------------------------------------------------
  bool beginCommands(std::string* error);
  void dispatch(const Pipeline& pipeline, uint32_t groups_x, uint32_t groups_y = 1,
                const void* push_constants = nullptr, uint32_t push_size = 0);
  /// Full compute-to-compute barrier. The stages here are short and the
  /// dependencies between them are total, so finer tracking would buy nothing.
  void barrier();
  void fill(const Buffer& buffer, uint32_t value, VkDeviceSize size = VK_WHOLE_SIZE,
            VkDeviceSize offset = 0);
  void copy(const Buffer& from, const Buffer& to, VkDeviceSize size);
  /// Submits and blocks. Returns false if the device was lost.
  bool submitAndWait(std::string* error);

#define PARALLAX_VK_DECLARE(name) PFN_##name name = nullptr;
  PARALLAX_VK_GLOBAL_FUNCTIONS(PARALLAX_VK_DECLARE)
  PARALLAX_VK_INSTANCE_FUNCTIONS(PARALLAX_VK_DECLARE)
  PARALLAX_VK_DEVICE_FUNCTIONS(PARALLAX_VK_DECLARE)
#undef PARALLAX_VK_DECLARE

  VkDevice device() const { return device_; }

 private:
  Context() = default;
  uint32_t findMemoryType(uint32_t allowed, VkMemoryPropertyFlags required, bool* found) const;

  void* library_ = nullptr;
  PFN_vkGetInstanceProcAddr get_instance_proc_ = nullptr;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_properties_{};
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  std::string device_name_;
  uint32_t max_workgroup_invocations_ = 0;
  VkDeviceSize max_storage_buffer_range_ = 0;
  bool recording_ = false;
};

}  // namespace vk
}  // namespace parallax

#endif  // __ANDROID__
