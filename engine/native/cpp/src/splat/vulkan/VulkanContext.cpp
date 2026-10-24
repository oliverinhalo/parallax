#include "VulkanContext.h"

#if defined(__ANDROID__)

#include <dlfcn.h>

#include <cstring>

namespace parallax {
namespace vk {
namespace {

constexpr uint32_t kDescriptorPoolSets = 64;
constexpr uint32_t kDescriptorPoolBuffers = 1024;
/// How long a single submission may take before it is treated as a hang. A
/// densification interval's worth of dispatches on a slow phone is well under
/// a second; ten is generous and still bounded.
constexpr uint64_t kFenceTimeoutNanoseconds = 10ull * 1000 * 1000 * 1000;

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

}  // namespace

Buffer::~Buffer() {
  if (context_ == nullptr) return;
  if (mapped_ != nullptr) context_->vkUnmapMemory(context_->device(), memory_);
  if (handle_ != VK_NULL_HANDLE) context_->vkDestroyBuffer(context_->device(), handle_, nullptr);
  if (memory_ != VK_NULL_HANDLE) context_->vkFreeMemory(context_->device(), memory_, nullptr);
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this == &other) return *this;
  this->~Buffer();
  context_ = other.context_;
  handle_ = other.handle_;
  memory_ = other.memory_;
  size_ = other.size_;
  mapped_ = other.mapped_;
  coherent_ = other.coherent_;
  other.context_ = nullptr;
  other.handle_ = VK_NULL_HANDLE;
  other.memory_ = VK_NULL_HANDLE;
  other.mapped_ = nullptr;
  other.size_ = 0;
  return *this;
}

Context::~Context() {
  if (device_ != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device_);
    if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
    if (descriptor_pool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    }
    if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
  }
  if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
  if (library_ != nullptr) dlclose(library_);
}

std::unique_ptr<Context> Context::create(std::string* error) {
  std::unique_ptr<Context> context(new Context());

  context->library_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (context->library_ == nullptr) {
    context->library_ = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  }
  if (context->library_ == nullptr) {
    fail(error, "this device has no Vulkan driver");
    return nullptr;
  }
  context->get_instance_proc_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(context->library_, "vkGetInstanceProcAddr"));
  if (context->get_instance_proc_ == nullptr) {
    fail(error, "the Vulkan loader is missing vkGetInstanceProcAddr");
    return nullptr;
  }

#define PARALLAX_VK_LOAD_GLOBAL(name)                                                      \
  context->name = reinterpret_cast<PFN_##name>(context->get_instance_proc_(nullptr, #name));
  PARALLAX_VK_GLOBAL_FUNCTIONS(PARALLAX_VK_LOAD_GLOBAL)
#undef PARALLAX_VK_LOAD_GLOBAL
  if (context->vkCreateInstance == nullptr) {
    fail(error, "the Vulkan loader is incomplete");
    return nullptr;
  }

  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.pApplicationName = "Parallax";
  application.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instance_info{};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &application;
  if (context->vkCreateInstance(&instance_info, nullptr, &context->instance_) != VK_SUCCESS) {
    // Falling back to 1.0 is worth a try: some drivers refuse a 1.1 request
    // outright even though everything used here is core 1.0.
    application.apiVersion = VK_API_VERSION_1_0;
    if (context->vkCreateInstance(&instance_info, nullptr, &context->instance_) != VK_SUCCESS) {
      fail(error, "could not create a Vulkan instance");
      return nullptr;
    }
  }

#define PARALLAX_VK_LOAD_INSTANCE(name)                                   \
  context->name = reinterpret_cast<PFN_##name>(                           \
      context->get_instance_proc_(context->instance_, #name));            \
  if (context->name == nullptr) {                                         \
    fail(error, std::string("Vulkan driver is missing ") + #name);        \
    return nullptr;                                                       \
  }
  PARALLAX_VK_INSTANCE_FUNCTIONS(PARALLAX_VK_LOAD_INSTANCE)
#undef PARALLAX_VK_LOAD_INSTANCE

  uint32_t device_count = 0;
  context->vkEnumeratePhysicalDevices(context->instance_, &device_count, nullptr);
  if (device_count == 0) {
    fail(error, "no Vulkan device is available");
    return nullptr;
  }
  std::vector<VkPhysicalDevice> devices(device_count);
  context->vkEnumeratePhysicalDevices(context->instance_, &device_count, devices.data());

  // Prefer a discrete device if one is somehow present, otherwise the first
  // with a compute queue; on a phone that is the only one there is.
  uint32_t chosen_family = 0;
  for (VkPhysicalDevice candidate : devices) {
    uint32_t family_count = 0;
    context->vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    context->vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
    for (uint32_t i = 0; i < family_count; ++i) {
      if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) continue;
      context->physical_device_ = candidate;
      chosen_family = i;
      break;
    }
    if (context->physical_device_ != VK_NULL_HANDLE) break;
  }
  if (context->physical_device_ == VK_NULL_HANDLE) {
    fail(error, "no Vulkan device exposes a compute queue");
    return nullptr;
  }
  context->queue_family_ = chosen_family;

  VkPhysicalDeviceProperties properties{};
  context->vkGetPhysicalDeviceProperties(context->physical_device_, &properties);
  context->device_name_ = properties.deviceName;
  context->max_workgroup_invocations_ = properties.limits.maxComputeWorkGroupInvocations;
  context->max_storage_buffer_range_ = properties.limits.maxStorageBufferRange;
  if (context->max_workgroup_invocations_ < 256) {
    fail(error, "the compute queue cannot run a 256-invocation workgroup");
    return nullptr;
  }
  context->vkGetPhysicalDeviceMemoryProperties(context->physical_device_,
                                               &context->memory_properties_);

  const float priority = 1.f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = chosen_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  if (context->vkCreateDevice(context->physical_device_, &device_info, nullptr,
                              &context->device_) != VK_SUCCESS) {
    fail(error, "could not create a Vulkan logical device");
    return nullptr;
  }

#define PARALLAX_VK_LOAD_DEVICE(name)                                        \
  context->name = reinterpret_cast<PFN_##name>(                              \
      context->vkGetDeviceProcAddr(context->device_, #name));                \
  if (context->name == nullptr) {                                            \
    fail(error, std::string("Vulkan driver is missing ") + #name);           \
    return nullptr;                                                          \
  }
  PARALLAX_VK_DEVICE_FUNCTIONS(PARALLAX_VK_LOAD_DEVICE)
#undef PARALLAX_VK_LOAD_DEVICE

  context->vkGetDeviceQueue(context->device_, chosen_family, 0, &context->queue_);

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = chosen_family;
  if (context->vkCreateCommandPool(context->device_, &pool_info, nullptr,
                                   &context->command_pool_) != VK_SUCCESS) {
    fail(error, "could not create a command pool");
    return nullptr;
  }

  VkCommandBufferAllocateInfo command_info{};
  command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_info.commandPool = context->command_pool_;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  if (context->vkAllocateCommandBuffers(context->device_, &command_info,
                                        &context->command_buffer_) != VK_SUCCESS) {
    fail(error, "could not allocate a command buffer");
    return nullptr;
  }

  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_size.descriptorCount = kDescriptorPoolBuffers;
  VkDescriptorPoolSize uniform_size{};
  uniform_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uniform_size.descriptorCount = kDescriptorPoolSets;
  const VkDescriptorPoolSize sizes[] = {pool_size, uniform_size};
  VkDescriptorPoolCreateInfo descriptor_info{};
  descriptor_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_info.maxSets = kDescriptorPoolSets;
  descriptor_info.poolSizeCount = 2;
  descriptor_info.pPoolSizes = sizes;
  if (context->vkCreateDescriptorPool(context->device_, &descriptor_info, nullptr,
                                      &context->descriptor_pool_) != VK_SUCCESS) {
    fail(error, "could not create a descriptor pool");
    return nullptr;
  }

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (context->vkCreateFence(context->device_, &fence_info, nullptr, &context->fence_) !=
      VK_SUCCESS) {
    fail(error, "could not create a fence");
    return nullptr;
  }
  return context;
}

uint32_t Context::findMemoryType(uint32_t allowed, VkMemoryPropertyFlags required,
                                 bool* found) const {
  for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
    if ((allowed & (1u << i)) == 0) continue;
    if ((memory_properties_.memoryTypes[i].propertyFlags & required) != required) continue;
    *found = true;
    return i;
  }
  *found = false;
  return 0;
}

Buffer Context::createBuffer(VkDeviceSize size, bool host_visible, std::string* error) const {
  Buffer buffer;
  if (size == 0) size = 4;
  if (size > max_storage_buffer_range_) {
    fail(error, "the scene needs a larger storage buffer than this GPU allows");
    return buffer;
  }

  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = size;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer handle = VK_NULL_HANDLE;
  if (vkCreateBuffer(device_, &info, nullptr, &handle) != VK_SUCCESS) {
    fail(error, "out of GPU memory creating a buffer");
    return buffer;
  }

  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device_, handle, &requirements);
  // Mobile GPUs share memory with the CPU, so host-visible is not the
  // penalty it is on a discrete card; the parameter buffers still ask for
  // device-local because they are read every dispatch.
  const VkMemoryPropertyFlags wanted =
      host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                   : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  bool found = false;
  uint32_t type = findMemoryType(requirements.memoryTypeBits, wanted, &found);
  bool coherent = host_visible;
  if (!found && host_visible) {
    type = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                          &found);
    coherent = false;
  }
  if (!found) {
    type = findMemoryType(requirements.memoryTypeBits, 0, &found);
  }
  if (!found) {
    vkDestroyBuffer(device_, handle, nullptr);
    fail(error, "no suitable GPU memory type");
    return buffer;
  }

  VkMemoryAllocateInfo allocation{};
  allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = type;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (vkAllocateMemory(device_, &allocation, nullptr, &memory) != VK_SUCCESS) {
    vkDestroyBuffer(device_, handle, nullptr);
    fail(error, "out of GPU memory");
    return buffer;
  }
  if (vkBindBufferMemory(device_, handle, memory, 0) != VK_SUCCESS) {
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyBuffer(device_, handle, nullptr);
    fail(error, "could not bind buffer memory");
    return buffer;
  }

  buffer.context_ = this;
  buffer.handle_ = handle;
  buffer.memory_ = memory;
  buffer.size_ = size;
  buffer.coherent_ = coherent;
  if (host_visible) {
    if (vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped_) != VK_SUCCESS) {
      buffer.mapped_ = nullptr;
      fail(error, "could not map host-visible memory");
      return Buffer{};
    }
  }
  return buffer;
}

bool Context::upload(const Buffer& buffer, const void* data, VkDeviceSize size,
                     VkDeviceSize offset) const {
  if (buffer.mapped() == nullptr || offset + size > buffer.size()) return false;
  std::memcpy(static_cast<uint8_t*>(buffer.mapped()) + offset, data,
              static_cast<size_t>(size));
  if (!buffer.coherent_) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = buffer.memory_;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(device_, 1, &range);
  }
  return true;
}

bool Context::download(const Buffer& buffer, void* data, VkDeviceSize size,
                       VkDeviceSize offset) const {
  if (buffer.mapped() == nullptr || offset + size > buffer.size()) return false;
  if (!buffer.coherent_) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = buffer.memory_;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(device_, 1, &range);
  }
  std::memcpy(data, static_cast<const uint8_t*>(buffer.mapped()) + offset,
              static_cast<size_t>(size));
  return true;
}

bool Context::createPipeline(const uint32_t* spirv, size_t word_count,
                             uint32_t push_constant_size, const std::vector<Binding>& bindings,
                             Pipeline* out, std::string* error) const {
  VkShaderModuleCreateInfo module_info{};
  module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  module_info.codeSize = word_count * sizeof(uint32_t);
  module_info.pCode = spirv;
  if (vkCreateShaderModule(device_, &module_info, nullptr, &out->module) != VK_SUCCESS) {
    return fail(error, "the driver rejected a compute shader");
  }

  std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
  layout_bindings.reserve(bindings.size());
  for (const Binding& binding : bindings) {
    VkDescriptorSetLayoutBinding entry{};
    entry.binding = binding.index;
    entry.descriptorType = binding.type;
    entry.descriptorCount = 1;
    entry.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    layout_bindings.push_back(entry);
  }
  VkDescriptorSetLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = static_cast<uint32_t>(layout_bindings.size());
  layout_info.pBindings = layout_bindings.data();
  if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &out->set_layout) !=
      VK_SUCCESS) {
    return fail(error, "could not create a descriptor set layout");
  }

  VkPushConstantRange range{};
  range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  range.offset = 0;
  range.size = push_constant_size;
  VkPipelineLayoutCreateInfo pipeline_layout{};
  pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout.setLayoutCount = 1;
  pipeline_layout.pSetLayouts = &out->set_layout;
  pipeline_layout.pushConstantRangeCount = push_constant_size > 0 ? 1 : 0;
  pipeline_layout.pPushConstantRanges = push_constant_size > 0 ? &range : nullptr;
  if (vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &out->layout) != VK_SUCCESS) {
    return fail(error, "could not create a pipeline layout");
  }
  out->push_constant_size = push_constant_size;

  VkComputePipelineCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  create.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  create.stage.module = out->module;
  create.stage.pName = "main";
  create.layout = out->layout;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &out->pipeline) !=
      VK_SUCCESS) {
    return fail(error, "could not compile a compute pipeline");
  }

  VkDescriptorSetAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = descriptor_pool_;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &out->set_layout;
  if (vkAllocateDescriptorSets(device_, &allocate, &out->descriptor_set) != VK_SUCCESS) {
    return fail(error, "could not allocate a descriptor set");
  }

  std::vector<VkDescriptorBufferInfo> buffer_infos(bindings.size());
  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(bindings.size());
  for (size_t i = 0; i < bindings.size(); ++i) {
    if (bindings[i].buffer == nullptr || !bindings[i].buffer->valid()) {
      return fail(error, "a shader binding was left unattached");
    }
    buffer_infos[i].buffer = bindings[i].buffer->handle();
    buffer_infos[i].offset = 0;
    buffer_infos[i].range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = out->descriptor_set;
    write.dstBinding = bindings[i].index;
    write.descriptorCount = 1;
    write.descriptorType = bindings[i].type;
    write.pBufferInfo = &buffer_infos[i];
    writes.push_back(write);
  }
  vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
  return true;
}

void Context::destroyPipeline(Pipeline* pipeline) const {
  if (pipeline->pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline->pipeline, nullptr);
  if (pipeline->layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline->layout, nullptr);
  if (pipeline->set_layout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device_, pipeline->set_layout, nullptr);
  }
  if (pipeline->module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, pipeline->module, nullptr);
  *pipeline = Pipeline{};
}

bool Context::beginCommands(std::string* error) {
  if (recording_) return true;
  vkResetCommandBuffer(command_buffer_, 0);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command_buffer_, &begin) != VK_SUCCESS) {
    return fail(error, "could not begin recording GPU commands");
  }
  recording_ = true;
  return true;
}

void Context::dispatch(const Pipeline& pipeline, uint32_t groups_x, uint32_t groups_y,
                       const void* push_constants, uint32_t push_size) {
  if (!recording_ || groups_x == 0 || groups_y == 0) return;
  vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
  vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1,
                          &pipeline.descriptor_set, 0, nullptr);
  if (push_constants != nullptr && push_size > 0) {
    vkCmdPushConstants(command_buffer_, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       push_size, push_constants);
  }
  vkCmdDispatch(command_buffer_, groups_x, groups_y, 1);
}

void Context::barrier() {
  if (!recording_) return;
  VkMemoryBarrier memory{};
  memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(command_buffer_,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       1, &memory, 0, nullptr, 0, nullptr);
}

void Context::fill(const Buffer& buffer, uint32_t value, VkDeviceSize size, VkDeviceSize offset) {
  if (!recording_ || !buffer.valid()) return;
  vkCmdFillBuffer(command_buffer_, buffer.handle(), offset, size, value);
}

void Context::copy(const Buffer& from, const Buffer& to, VkDeviceSize size) {
  if (!recording_ || !from.valid() || !to.valid()) return;
  VkBufferCopy region{};
  region.size = size;
  vkCmdCopyBuffer(command_buffer_, from.handle(), to.handle(), 1, &region);
}

bool Context::submitAndWait(std::string* error) {
  if (!recording_) return true;
  recording_ = false;
  if (vkEndCommandBuffer(command_buffer_) != VK_SUCCESS) {
    return fail(error, "could not finish recording GPU commands");
  }
  vkResetFences(device_, 1, &fence_);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer_;
  if (vkQueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS) {
    return fail(error, "the GPU rejected the submission");
  }
  const VkResult waited = vkWaitForFences(device_, 1, &fence_, VK_TRUE, kFenceTimeoutNanoseconds);
  if (waited == VK_TIMEOUT) return fail(error, "the GPU stopped responding");
  if (waited != VK_SUCCESS) return fail(error, "the GPU reported a device loss");
  return true;
}

}  // namespace vk
}  // namespace parallax

#endif  // __ANDROID__
