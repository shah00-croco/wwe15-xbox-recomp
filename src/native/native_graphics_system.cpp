// Native renderer GPU plugin, rexgpu-native.dll (docs/native-renderer.md,
// Phase 5). Loaded instead of the xenos plugin with --gpu_plugin=native.
//
// It reuses the ReXGlue SDK's backend-independent GraphicsSystem and
// CommandProcessor, compiled from the SDK sources. They keep consuming the
// guest's ring buffer, so D3D's fences, interrupts, vblank and swaps behave as
// before. The Xenos draw and resolve emulation is left out.
//
// M3 work in progress:
// - every swap fills the presenter's guest output image through Vulkan (a test
//   frame for now), proving the native presentation path;
// - --wwe2k15_native_trace_frame=N writes the state of every draw of guest
//   frame N to --wwe2k15_native_trace_file, to decide the features needed.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

REXCVAR_DEFINE_INT32(wwe2k15_native_trace_frame, 0, "Native",
                     "Guest frame whose draws are written to wwe2k15_native_trace_file (0: off)");
REXCVAR_DEFINE_STRING(wwe2k15_native_trace_file, "", "Native",
                      "Output file for --wwe2k15_native_trace_frame");

namespace wwe2k15::native {

using rex::graphics::CommandProcessor;
using rex::graphics::GraphicsSystem;
using rex::graphics::Shader;
namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;
namespace vk_ui = rex::ui::vulkan;

class NativeCommandProcessor : public CommandProcessor {
 public:
  NativeCommandProcessor(GraphicsSystem* graphics_system, rex::system::KernelState* kernel_state)
      : CommandProcessor(graphics_system, kernel_state) {}

  // The 256-entry gamma ramp (DC_LUT_30_COLOR words) the guest wrote through
  // the DC_LUT_* registers, for the native present (section 20.7).
  const uint32_t* gamma_ramp_words() const {
    return reinterpret_cast<const uint32_t*>(gamma_ramp_256_entry_table());
  }

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override {
    ++swaps_;
    if (trace_) {
      std::fclose(trace_);
      trace_ = nullptr;
      REXGPU_INFO("[native] draw trace of frame {} written", swaps_ - 1);
    }
    if (REXCVAR_GET(wwe2k15_native_trace_frame) > 0 &&
        swaps_ == uint64_t(REXCVAR_GET(wwe2k15_native_trace_frame))) {
      trace_ = std::fopen(REXCVAR_GET(wwe2k15_native_trace_file).c_str(), "w");
    }
    if (swaps_ == 1 || swaps_ % 600 == 0) {
      REXGPU_INFO("[native] swap {}: {}x{} front buffer {:08X}, {} draws ({} copy mode) "
                  "since the last report, {} shaders loaded",
                  swaps_, frontbuffer_width, frontbuffer_height, frontbuffer_ptr, draws_,
                  copy_draws_, shaders_.size());
      draws_ = copy_draws_ = 0;
    }
    // Presentation is done by the game-side renderer core
    // (src/native/native_vulkan.cpp) at the guest Present hook.
  }

 protected:
  bool SetupContext() override { return true; }

  void ShutdownContext() override {
    if (trace_) {
      std::fclose(trace_);
      trace_ = nullptr;
    }
    if (vk_device_) {
      const auto& dfn = vk_device_->functions();
      if (fence_) dfn.vkDestroyFence(vk_device_->device(), fence_, nullptr);
      if (command_pool_) dfn.vkDestroyCommandPool(vk_device_->device(), command_pool_, nullptr);
    }
    shaders_.clear();
  }

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t /*guest_address*/,
                     const uint32_t* host_address, uint32_t dword_count) override {
    // FNV-1a over the big-endian ucode identifies the shader.
    uint64_t hash = 0xCBF29CE484222325ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(host_address);
    for (size_t i = 0; i < size_t(dword_count) * 4; ++i) {
      hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    }
    auto& shader = shaders_[hash];
    if (!shader) {
      shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
      // The bindings (textures, vertex fetches) are needed for draw tracing.
      shader->AnalyzeUcode(ucode_disasm_);
    }
    return shader.get();
  }

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool /*major_mode_explicit*/) override {
    ++draws_;
    const auto& regs = *register_file_;
    const auto mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
    if (mode == xenos::EdramMode::kCopy) {
      ++copy_draws_;
    }
    if (trace_) {
      TraceDraw(prim_type, index_count, index_buffer_info);
    }
    return true;
  }

  bool IssueCopy() override { return true; }

 private:
  void TraceDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 const IndexBufferInfo* index_buffer_info) {
    const auto& regs = *register_file_;
    const auto color_info = regs.Get<reg::RB_COLOR_INFO>();
    const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
    const auto blend = regs.Get<reg::RB_BLENDCONTROL>();  // render target 0
    const auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
    const auto depth = regs.Get<reg::RB_DEPTHCONTROL>();
    const Shader* vs = active_vertex_shader();
    const Shader* ps = active_pixel_shader();
    std::fprintf(trace_,
                 "DRAW prim=%u count=%u indexed=%d mode=%u rt0_fmt=%u rt0_base=%u pitch=%u msaa=%u "
                 "blend=%08X alphatest=%u/%u z=%u/%u vs=%016llX ps=%016llX\n",
                 uint32_t(prim_type), index_count, index_buffer_info ? 1 : 0,
                 uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode),
                 uint32_t(color_info.color_format), uint32_t(color_info.color_base),
                 uint32_t(surface.surface_pitch), uint32_t(surface.msaa_samples), blend.value,
                 uint32_t(colorcontrol.alpha_test_enable), uint32_t(colorcontrol.alpha_func),
                 uint32_t(depth.z_enable), uint32_t(depth.z_write_enable),
                 (unsigned long long)(vs ? vs->ucode_data_hash() : 0),
                 (unsigned long long)(ps ? ps->ucode_data_hash() : 0));
    for (const Shader* shader : {vs, ps}) {
      if (!shader) continue;
      for (const auto& binding : shader->texture_bindings()) {
        const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
        std::fprintf(trace_,
                     "  TEX %s fc=%u fmt=%u dim=%u tiled=%u %ux%u base=%08X endian=%u type=%u\n",
                     shader == vs ? "vs" : "ps", binding.fetch_constant, uint32_t(fetch.format),
                     uint32_t(fetch.dimension), uint32_t(fetch.tiled),
                     uint32_t(fetch.size_2d.width) + 1, uint32_t(fetch.size_2d.height) + 1,
                     uint32_t(fetch.base_address) << 12, uint32_t(fetch.endianness),
                     uint32_t(fetch.type));
      }
      for (const auto& binding : shader->vertex_bindings()) {
        const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
        std::fprintf(trace_, "  VTX fc=%u address=%08X size=%u stride=%u attributes=%zu\n",
                     binding.fetch_constant, uint32_t(fetch.address) << 2,
                     uint32_t(fetch.size) * 4, binding.stride_words * 4,
                     binding.attributes.size());
      }
    }
  }

  // Fills the presenter's guest output image with a colour that changes with the
  // frame count. Simple and synchronous; replaced by real drawing in M3.
  void PresentTestFrame(uint32_t width, uint32_t height) {
    auto* presenter = graphics_system_->presenter();
    auto* provider = static_cast<vk_ui::VulkanProvider*>(graphics_system_->provider());
    if (!presenter || !provider || !provider->vulkan_device()) {
      return;
    }
    if (!EnsureVulkanObjects(provider->vulkan_device())) {
      return;
    }
    const float pulse = float(swaps_ % 120) / 119.0f;
    presenter->RefreshGuestOutput(
        width, height, width, height, [this, pulse](rex::ui::Presenter::GuestOutputRefreshContext& ctx) {
          auto& vk_ctx =
              static_cast<vk_ui::VulkanPresenter::VulkanGuestOutputRefreshContext&>(ctx);
          const auto& dfn = vk_device_->functions();
          const VkDevice device = vk_device_->device();
          dfn.vkResetCommandPool(device, command_pool_, 0);
          VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
          begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
          dfn.vkBeginCommandBuffer(command_buffer_, &begin);
          VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          barrier.srcAccessMask = 0;
          barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = vk_ctx.image();
          barrier.subresourceRange = range;
          dfn.vkCmdPipelineBarrier(command_buffer_,
                                   vk_ctx.image_ever_written_previously()
                                       ? vk_ui::VulkanPresenter::kGuestOutputInternalStageMask
                                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                   &barrier);
          VkClearColorValue color{};
          color.float32[0] = 0.10f;
          color.float32[1] = 0.10f + 0.25f * pulse;
          color.float32[2] = 0.35f;
          color.float32[3] = 1.0f;
          dfn.vkCmdClearColorImage(command_buffer_, vk_ctx.image(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
          barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          barrier.dstAccessMask = vk_ui::VulkanPresenter::kGuestOutputInternalAccessMask;
          barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          barrier.newLayout = vk_ui::VulkanPresenter::kGuestOutputInternalLayout;
          dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   vk_ui::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0,
                                   nullptr, 0, nullptr, 1, &barrier);
          dfn.vkEndCommandBuffer(command_buffer_);
          VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
          submit.commandBufferCount = 1;
          submit.pCommandBuffers = &command_buffer_;
          dfn.vkResetFences(device, 1, &fence_);
          {
            auto queue = vk_device_->AcquireQueue(vk_device_->queue_family_graphics_compute(), 0);
            if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence_) != VK_SUCCESS) {
              return false;
            }
          }
          // Synchronous for the test frame; real drawing will use a submission ring.
          dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
          return true;
        });
  }

  bool EnsureVulkanObjects(const vk_ui::VulkanDevice* device) {
    if (vk_device_) {
      return true;
    }
    const auto& dfn = device->functions();
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(device->device(), &pool_info, nullptr, &command_pool_) !=
        VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (dfn.vkAllocateCommandBuffers(device->device(), &alloc, &command_buffer_) != VK_SUCCESS ||
        dfn.vkCreateFence(device->device(), &fence_info, nullptr, &fence_) != VK_SUCCESS) {
      return false;
    }
    vk_device_ = device;
    REXGPU_INFO("[native] Vulkan presentation objects created");
    return true;
  }

  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
  rex::string::StringBuffer ucode_disasm_;
  uint64_t swaps_ = 0;
  uint64_t draws_ = 0;
  uint64_t copy_draws_ = 0;
  std::FILE* trace_ = nullptr;

  const vk_ui::VulkanDevice* vk_device_ = nullptr;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
};

class NativeGraphicsSystem : public GraphicsSystem {
 public:
  std::string name() const override { return "Native"; }

 protected:
  void CreateProvider(bool with_presentation) override {
    // No GPU emulation: the provider only serves presentation and our renderer.
    provider_ = vk_ui::VulkanProvider::Create(false, with_presentation);
  }

  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace wwe2k15::native

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion || !info ||
      info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-native: ABI mismatch or invalid GpuCreateInfo");
    return nullptr;
  }
  const std::string_view backend = info->backend ? info->backend : "any";
  if (backend != "any" && backend != "vulkan") {
    REXLOG_ERROR("rexgpu-native: only the Vulkan backend exists, '{}' was requested", backend);
    return nullptr;
  }
  return new wwe2k15::native::NativeGraphicsSystem();
}

// For the game-side renderer core (src/native/native_vulkan.cpp), which finds
// it with GetProcAddress: the current 256-entry gamma ramp, or null before the
// command processor exists.
extern "C" REX_GPU_PLUGIN_EXPORT const uint32_t* wwe2k15_native_gamma_ramp(
    rex::system::IGraphicsSystem* graphics_system) {
  auto* graphics = static_cast<rex::graphics::GraphicsSystem*>(graphics_system);
  auto* processor = graphics ? static_cast<wwe2k15::native::NativeCommandProcessor*>(
                                   graphics->command_processor())
                             : nullptr;
  return processor ? processor->gamma_ramp_words() : nullptr;
}
