// rexgpu-xenosprof.dll: the SDK's Vulkan GPU emulation built from the SDK
// sources with debug symbols, only for profiling (docs/native-renderer.md,
// section 15). Same code as rexgpu-xenos, Vulkan backend only.
// Use with --gpu_plugin=xenosprof.

#include <cstdint>
#include <string_view>

#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion || !info ||
      info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-xenosprof: ABI mismatch or invalid GpuCreateInfo");
    return nullptr;
  }
  const std::string_view backend = info->backend ? info->backend : "any";
  if (backend != "any" && backend != "vulkan") {
    REXLOG_ERROR("rexgpu-xenosprof: only the Vulkan backend is built, '{}' was requested",
                 backend);
    return nullptr;
  }
  return new rex::graphics::vulkan::VulkanGraphicsSystem();
}
