// Native renderer, Vulkan core (docs/native-renderer.md, sections 19.4 and 20).
//
// Runs in the game process on the SDK's Vulkan device (the one the presenter
// uses), reached through rex::Runtime::instance()->graphics_system(). Only
// active with --wwe2k15_native_renderer=true together with --gpu_plugin=native,
// where nothing else draws.
//
// Frame flow: draws from the game's D3D calls (native_renderer.cpp) are
// recorded into one command buffer, inside a render pass on the renderer's
// back buffer. At the guest Present the pass ends and the back buffer is drawn
// into the presenter's guest output image with a full-screen triangle (that
// image has no TRANSFER_DST usage, so it must be rendered to).
//
// Shaders are XenosRecomp output with the WWE 2K15 patch (section 19.3):
// set 0 = vertex/pixel/shared constants as storage buffers (dynamic offsets),
// set 1 = 32 slots each of 2D/3D/cube textures and samplers, one per texture
// fetch constant.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "native_draw.h"
#include "shaders/present_blit_spirv.h"

REXCVAR_DEFINE_BOOL(wwe2k15_native_renderer, false, "Native",
                    "Draw with the native renderer (requires --gpu_plugin=native)");
REXCVAR_DEFINE_INT32(wwe2k15_native_dump_resolves, 0, "Native",
                     "Save every resolve destination and the back buffer of this frame as "
                     "BMP files in native-dumps/frame-N (0: off)");
REXCVAR_DEFINE_STRING(wwe2k15_native_dump_address, "", "Native",
                      "Save every color resolve to this hex address (as the resolve log "
                      "prints it), in any frame, to native-dumps/frame-N");
REXCVAR_DEFINE_BOOL(wwe2k15_native_msaa, true, "Native",
                    "Draw anti-aliased render targets with 2x/4x multisampling, as the game "
                    "asks (off: single-sampled)");
REXCVAR_DEFINE_BOOL(wwe2k15_native_mips, true, "Native",
                    "Load texture mip levels (off: the base level only)");
REXCVAR_DEFINE_BOOL(wwe2k15_native_stream_cache, true, "Native",
                    "Keep converted vertex buffers between draws and frames while their "
                    "contents stay the same (off: convert every draw)");
REXCVAR_DEFINE_STRING(wwe2k15_native_shader_dir, "", "Native",
                      "Folder with <container hash>.spv shaders from XenosRecomp "
                      "(default: <exe dir>/../../native-shaders)");

namespace wwe2k15::native {

namespace vk_ui = rex::ui::vulkan;
namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;

namespace {

constexpr uint32_t kBackBufferWidth = 1280;
constexpr uint32_t kBackBufferHeight = 720;
constexpr VkFormat kBackBufferFormat = VK_FORMAT_R8G8B8A8_UNORM;
// Every render target has a depth buffer in this format (section 20.10).
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
constexpr VkFormat kDepthTextureFormat = VK_FORMAT_R32_SFLOAT;  // depth resolves
constexpr VkDeviceSize kUploadRingSize = 32 * 1024 * 1024;
constexpr VkDeviceSize kConstantsRange = 4096;       // 256 float4
constexpr VkDeviceSize kGammaRampSize = 256 * sizeof(uint32_t);
constexpr VkDeviceSize kSharedConstantsRange = 2048;  // see section 18
constexpr uint32_t kFetchSlots = 32;
constexpr uint32_t kMaxTargetWidth = 2048, kMaxTargetHeight = 1024;

// Vulkan format of an EDRAM color format (section 20.11). Gamma targets stay
// UNORM (the shaders already write gamma-space values there, as on the GPU).
// The 16-bit fixed formats hold -32...32: half floats keep that range (the
// 7e3 float format, [0, 32), is approximated with them too).
VkFormat TargetFormat(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  switch (format) {
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_16_16:
    case F::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case F::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return kBackBufferFormat;
  }
}

bool IsFixed16(xenos::ColorRenderTargetFormat format) {
  return format == xenos::ColorRenderTargetFormat::k_16_16 ||
         format == xenos::ColorRenderTargetFormat::k_16_16_16_16;
}

// RB_COLOR_INFO's color_exp_bias (signed, bits 20-25): the pixel shader's
// output is multiplied by 2^bias before it is stored (g_OutputScale).
float OutputScale(uint32_t color_info) {
  reg::RB_COLOR_INFO color;
  color.value = color_info;
  return std::ldexp(1.0f, int32_t(color.color_exp_bias));
}

// The swizzle a resolve copy is sampled with. Textures in ARGB order (red
// read from Z, blue from X, as D3D's 0x60A) get the resolve to swap red and
// blue as it writes (RB_COPY_DEST_INFO's copy_dest_swap), so the two cancel:
// the copy keeps the target's order, and X and Z trade places here. Constant
// and repeated components (k_16_16's X,Y,1,1) are kept.
uint32_t ResolveSwizzle(uint32_t swizzle) {
  if ((swizzle & 7) != 2 || ((swizzle >> 6) & 7) != 0) return swizzle;
  uint32_t result = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t component = (swizzle >> (i * 3)) & 7;
    if (component == 0 || component == 2) component ^= 2;
    result |= component << (i * 3);
  }
  return result;
}

// The format a resolve of a target is sampled as. The GPU resolves the
// fixed formats to unsigned data, 0...1 of the -32...32 values (the SDK's
// IsColorResolveFormatBitwiseEquivalent note): a blit from the half float
// target to UNORM does that clamp. Everything else is copied as written.
VkFormat ResolvedFormat(VkFormat format, bool fixed16) {
  if (!fixed16) return format;
  return format == VK_FORMAT_R16G16_SFLOAT ? VK_FORMAT_R16G16_UNORM
                                           : VK_FORMAT_R16G16B16A16_UNORM;
}

uint32_t FormatTexelSize(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R32G32_SFLOAT:
      return 8;
    default:
      return 4;
  }
}

// D3DDECLUSAGE names as XenosRecomp writes them in its input semantics
// (USAGE_SEMANTICS, followed by the usage index), section 20.9.
constexpr const char* kUsageSemantics[] = {
    "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL",   "PSIZE", "TEXCOORD", "TANGENT",
    "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",    "FOG",   "DEPTH",    "SAMPLE"};

// A vertex shader input: its location and whether it is declared uint4
// (XenosRecomp's NORMAL, TANGENT, BINORMAL and BLENDINDICES).
struct ShaderInput {
  uint32_t location = 0;
  bool integer = false;
};
using ShaderInputs = std::unordered_map<std::string, ShaderInput>;

// The stage inputs of a SPIR-V module by semantic ("in.var.NORMAL0" ->
// "NORMAL0"). XenosRecomp assigns locations from a table made for Sonic
// Unleashed, and usages outside it get 16 and up in the shader's own order, so
// the locations are read back from each shader instead of from a copy of the
// table.
ShaderInputs ReflectInputs(const uint32_t* words, size_t count) {
  std::unordered_map<uint32_t, std::string> names;
  std::unordered_map<uint32_t, uint32_t> locations, pointee, vector_component, inputs;
  std::unordered_map<uint32_t, bool> integer_type;
  for (size_t i = 5; i < count;) {
    const uint32_t op = words[i] & 0xFFFF, length = words[i] >> 16;
    if (!length || i + length > count) break;
    const uint32_t* w = words + i;
    switch (op) {
      case 5:  // OpName
        if (length > 2) {
          const char* text = reinterpret_cast<const char*>(w + 2);
          names[w[1]].assign(text, strnlen(text, (length - 2) * 4));
        }
        break;
      case 71:  // OpDecorate ... Location
        if (length > 3 && w[2] == 30) locations[w[1]] = w[3];
        break;
      case 21: integer_type[w[1]] = true; break;   // OpTypeInt
      case 22: integer_type[w[1]] = false; break;  // OpTypeFloat
      case 23: vector_component[w[1]] = w[2]; break;  // OpTypeVector
      case 32: pointee[w[1]] = w[3]; break;           // OpTypePointer
      case 59:  // OpVariable, storage class Input
        if (length > 3 && w[3] == 1) inputs[w[2]] = w[1];
        break;
    }
    i += length;
  }
  ShaderInputs result;
  constexpr std::string_view kPrefix = "in.var.";
  for (const auto& [id, pointer_type] : inputs) {
    auto name = names.find(id);
    auto location = locations.find(id);
    if (name == names.end() || location == locations.end() ||
        !name->second.starts_with(kPrefix)) {
      continue;
    }
    uint32_t type = pointee[pointer_type];
    if (auto it = vector_component.find(type); it != vector_component.end()) type = it->second;
    result[name->second.substr(kPrefix.size())] = {location->second, integer_type[type]};
  }
  return result;
}

// Xbox D3DDECLTYPE to Vulkan. The low 6 bits are the Xenos vertex format, bit
// 8 is "signed" and bit 9 "integer" (not normalized); D3DCOLOR (0x182886)
// differs from UBYTE4N (0x1A2086) by its swizzle. The data is converted to
// little-endian 32-bit words on upload, which puts the first component in the
// low bits, as Xenos fetches it. integer_input: the shader declares it uint4
// and gets the bits (normals are unpacked in the shader, section 20.9).
VkFormat VertexFormat(uint32_t decl_type, bool integer_input) {
  const bool is_signed = decl_type & 0x100, is_integer = decl_type & 0x200;
  auto pick = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled) {
    return is_integer ? (is_signed ? sscaled : uscaled) : (is_signed ? snorm : unorm);
  };
  const uint32_t format = decl_type & 0x3F;
  if (integer_input) {
    switch (format) {
      case 6: return VK_FORMAT_R8G8B8A8_UINT;
      case 7: case 16: case 17: case 33: case 36: return VK_FORMAT_R32_UINT;
      case 25: return VK_FORMAT_R16G16_UINT;
      case 26: return VK_FORMAT_R16G16B16A16_UINT;
      case 34: case 37: return VK_FORMAT_R32G32_UINT;
      case 57: return VK_FORMAT_R32G32B32_UINT;
      case 35: case 38: return VK_FORMAT_R32G32B32A32_UINT;
      default: return VK_FORMAT_UNDEFINED;
    }
  }
  switch (format) {
    case 36: return VK_FORMAT_R32_SFLOAT;           // k_32_FLOAT
    case 37: return VK_FORMAT_R32G32_SFLOAT;        // k_32_32_FLOAT
    case 57: return VK_FORMAT_R32G32B32_SFLOAT;     // k_32_32_32_FLOAT
    case 38: return VK_FORMAT_R32G32B32A32_SFLOAT;  // k_32_32_32_32_FLOAT
    case 31: return VK_FORMAT_R16G16_SFLOAT;        // k_16_16_FLOAT
    case 32: return VK_FORMAT_R16G16B16A16_SFLOAT;  // k_16_16_16_16_FLOAT
    case 6:                                         // k_8_8_8_8
      if ((decl_type >> 16) == 0x18) return VK_FORMAT_B8G8R8A8_UNORM;  // D3DCOLOR
      return pick(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_USCALED,
                  VK_FORMAT_R8G8B8A8_SSCALED);
    case 7:  // k_2_10_10_10
      return pick(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                  VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32);
    case 25:  // k_16_16
      return pick(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                  VK_FORMAT_R16G16_SSCALED);
    case 26:  // k_16_16_16_16
      return pick(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                  VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED);
    default: return VK_FORMAT_UNDEFINED;
  }
}

// XenosRecomp's normal unpacking (tfetchR11G11B10) reads x and y as 11 bits
// and z as 10, from the low bits up: the Xenos k_10_11_11 layout. This game's
// normals are k_11_11_10 (x 10 bits, then y and z 11), so they are repacked on
// upload: x doubled to 11 bits, z halved to 10, all signed.
uint32_t RepackNormal(uint32_t value) {
  const uint32_t x = (value << 1) & 0x7FF, y = (value >> 10) & 0x7FF, z = (value >> 22) & 0x3FF;
  return x | (y << 11) | (z << 22);
}

uint64_t Fnv1a(const void* data, size_t size, uint64_t hash = 0xCBF29CE484222325ull) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001B3ull;
  return hash;
}

// Byte offset of block (x, y) in a tiled 2D texture; pitch in blocks. Same as
// the SDK's texture_util::GetTiledOffset2D, which the game target does not link.
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

uint32_t Log2Floor(uint32_t value) { return 31 - uint32_t(std::countl_zero(value | 1)); }
uint32_t Log2Ceil(uint32_t value) { return value <= 1 ? 0 : Log2Floor(value - 1) + 1; }
uint32_t NextPow2(uint32_t value) { return 1u << Log2Ceil(value); }

// The first level stored in the packed mip tail: the one whose shorter side is
// 16 texels or less. As the SDK's texture_util::GetPackedMipLevel.
uint32_t PackedMipLevel(uint32_t width, uint32_t height) {
  const uint32_t log2_size = Log2Ceil(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

// Texel offset of level inside the packed mip tail of a 2D texture, for a
// level at or after PackedMipLevel. As the SDK's
// texture_util::GetPackedMipOffset without the 3D case.
void PackedMipOffset(uint32_t width, uint32_t height, uint32_t level, uint32_t& x,
                     uint32_t& y) {
  const uint32_t log2_width = Log2Ceil(width), log2_height = Log2Ceil(height);
  const uint32_t log2_size = std::min(log2_width, log2_height);
  const uint32_t packed_base = log2_size > 4 ? log2_size - 4 : 0;
  const uint32_t packed = level - packed_base;
  x = y = 0;
  if (packed < 3) {
    // 16, 8 and 4 texels from the corner, across the shorter side.
    (log2_width > log2_height ? y : x) = 16 >> packed;
  } else if (log2_width > log2_height) {
    x = (1u << (log2_width - packed_base)) >> (packed - 2);
  } else {
    y = (1u << (log2_height - packed_base)) >> (packed - 2);
  }
}

// XenosRecomp's SPEC_CONSTANT_ALPHA_TEST and where its pixel shaders read
// g_AlphaThreshold in the shared constants (shader_common.h).
constexpr uint32_t kSpecPackedNormals = 1u << 0;  // SPEC_CONSTANT_R11G11B10_NORMAL
constexpr uint32_t kSpecAlphaTest = 1u << 1;

// A pipeline and the vertex elements its draws repack on upload.
struct Pipeline {
  struct Element {
    uint32_t stream, offset;
    uint32_t words = 1;
  };
  VkPipeline pipeline = VK_NULL_HANDLE;
  std::vector<Element> repacked_normals;
  // Elements with two 16-bit components per word (16_16, 16_16_16_16 and
  // their float variants): the first component is the high half of the
  // big-endian word, so the halves are swapped after the endian swap.
  std::vector<Element> swapped_halves;
};
constexpr size_t kBooleansOffset = 512;
// Resolve destinations come from the texture object, which holds the CPU
// address (0xE0000000-0xFFFFFFFF for render targets); fetch constants set for
// draws hold the physical address, which for that range is 4 KB higher. Both
// are keyed by physical 4 KB page.
uint32_t PhysicalPage(uint32_t page) {
  return (page & 0x1FFFF) + (page >= 0xE0000 ? 1 : 0);
}

constexpr size_t kHalfPixelOffsetOffset = 520;
constexpr size_t kAlphaThresholdOffset = 528;
constexpr size_t kPositionScaleOffset = 536;
constexpr size_t kOutputScaleOffset = 544;
constexpr size_t kLoopConstantsOffset = 1024;

enum class AlphaTest { kOff, kOn, kRejectAll };

// The shaders only implement clip(alpha - threshold), which is alpha >= ref.
// kGreater is treated the same; the other functions are not supported yet.
constexpr uint32_t kAlphaToMaskBit = 1u << 4;  // RB_COLORCONTROL alpha_to_mask_enable
constexpr uint32_t kCullBits = 0x7;  // PA_SU_SC_MODE_CNTL cull_front, cull_back, face
constexpr uint32_t kRestartBit = 1u << 21;  // PA_SU_SC_MODE_CNTL multi_prim_ib_ena

AlphaTest GetAlphaTest(const NativeDrawState& state) {
  reg::RB_COLORCONTROL color_control;
  color_control.value = state.color_control;
  if (!color_control.alpha_test_enable) return AlphaTest::kOff;
  switch (color_control.alpha_func) {
    case xenos::CompareFunction::kNever: return AlphaTest::kRejectAll;
    case xenos::CompareFunction::kGreater:
    case xenos::CompareFunction::kGreaterEqual: return AlphaTest::kOn;
    default: return AlphaTest::kOff;
  }
}

VkBlendFactor BlendFactor(xenos::BlendFactor factor) {
  switch (factor) {
    case xenos::BlendFactor::kZero: return VK_BLEND_FACTOR_ZERO;
    case xenos::BlendFactor::kOne: return VK_BLEND_FACTOR_ONE;
    case xenos::BlendFactor::kSrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case xenos::BlendFactor::kDstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    // The blend constants (RB_BLEND_RED..ALPHA) are not passed yet; they stay 0.
    case xenos::BlendFactor::kConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case xenos::BlendFactor::kOneMinusConstantColor:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case xenos::BlendFactor::kConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case xenos::BlendFactor::kSrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp BlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract: return VK_BLEND_OP_SUBTRACT;
    case xenos::BlendOp::kMin: return VK_BLEND_OP_MIN;
    case xenos::BlendOp::kMax: return VK_BLEND_OP_MAX;
    case xenos::BlendOp::kRevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

// RB_DEPTHCONTROL and RB_STENCILREFMASK(_BF) as Vulkan depth and stencil
// state. Xenos compare functions and stencil operations are numbered like
// VkCompareOp and VkStencilOp.
VkPipelineDepthStencilStateCreateInfo DepthStencilState(const NativeDrawState& state) {
  reg::RB_DEPTHCONTROL control;
  control.value = state.depth_control;
  VkPipelineDepthStencilStateCreateInfo info{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  info.depthTestEnable = control.z_enable;
  info.depthWriteEnable = control.z_enable && control.z_write_enable;
  info.depthCompareOp = VkCompareOp(control.zfunc);
  info.stencilTestEnable = control.stencil_enable;
  auto face = [](xenos::CompareFunction func, xenos::StencilOp fail, xenos::StencilOp zpass,
                 xenos::StencilOp zfail, uint32_t ref_mask) {
    reg::RB_STENCILREFMASK masks;
    masks.value = ref_mask;
    return VkStencilOpState{VkStencilOp(fail), VkStencilOp(zpass),  VkStencilOp(zfail),
                            VkCompareOp(func), masks.stencilmask,   masks.stencilwritemask,
                            masks.stencilref};
  };
  info.front = face(control.stencilfunc, control.stencilfail, control.stencilzpass,
                    control.stencilzfail, state.stencil_ref_mask);
  // Without backface_enable, back faces use the front-face state.
  info.back = control.backface_enable
                  ? face(control.stencilfunc_bf, control.stencilfail_bf, control.stencilzpass_bf,
                         control.stencilzfail_bf, state.stencil_ref_mask_bf)
                  : info.front;
  return info;
}

// RB_BLENDCONTROL0 and RB_COLOR_MASK (render target 0) as a Vulkan attachment
// state. Xenos has no blend enable bit: ONE/ZERO/ADD means no blending.
VkPipelineColorBlendAttachmentState BlendAttachment(const NativeDrawState& state) {
  reg::RB_BLENDCONTROL blend_control;
  blend_control.value = state.blend_control;
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.srcColorBlendFactor = BlendFactor(blend_control.color_srcblend);
  attachment.dstColorBlendFactor = BlendFactor(blend_control.color_destblend);
  attachment.colorBlendOp = BlendOp(blend_control.color_comb_fcn);
  attachment.srcAlphaBlendFactor = BlendFactor(blend_control.alpha_srcblend);
  attachment.dstAlphaBlendFactor = BlendFactor(blend_control.alpha_destblend);
  attachment.alphaBlendOp = BlendOp(blend_control.alpha_comb_fcn);
  const bool passthrough =
      attachment.srcColorBlendFactor == VK_BLEND_FACTOR_ONE &&
      attachment.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
      attachment.colorBlendOp == VK_BLEND_OP_ADD &&
      attachment.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
      attachment.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO &&
      attachment.alphaBlendOp == VK_BLEND_OP_ADD;
  attachment.blendEnable = passthrough ? VK_FALSE : VK_TRUE;
  attachment.colorWriteMask = state.color_mask & 0xF;  // write_red0..write_alpha0 = R, G, B, A
  return attachment;
}

struct Texture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

class VulkanCore {
 public:
  bool EnsureInitialized() {
    if (initialized_) {
      return device_ != nullptr;
    }
    initialized_ = true;
    auto* runtime = rex::Runtime::instance();
    auto* graphics = runtime ? runtime->graphics_system() : nullptr;
    auto* provider = graphics ? static_cast<vk_ui::VulkanProvider*>(graphics->provider()) : nullptr;
    presenter_ = graphics ? graphics->presenter() : nullptr;
    if (!provider || !provider->vulkan_device() || !presenter_) {
      REXGPU_ERROR("[native] no Vulkan device or presenter; the native renderer stays off");
      return false;
    }
    device_ = provider->vulkan_device();
    if (!CreateObjects()) {
      REXGPU_ERROR("[native] creating the Vulkan core objects failed");
      device_ = nullptr;
      return false;
    }
    shader_dir_ = REXCVAR_GET(wwe2k15_native_shader_dir);
    if (shader_dir_.empty()) {
      shader_dir_ = (std::filesystem::current_path() / ".." / ".." / "native-shaders").string();
    }
    REXGPU_INFO("[native] renderer core ready: back buffer {}x{}, shaders from {}",
                kBackBufferWidth, kBackBufferHeight, shader_dir_);
    return true;
  }

  void Draw(const NativeDrawState& state) {
    BeginFrameIfNeeded();
    ++frame_draws_;
    struct Timer {
      std::chrono::steady_clock::duration& total;
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      ~Timer() { total += std::chrono::steady_clock::now() - start; }
    } timer{draw_time_};
    VkPrimitiveTopology topology;
    bool quads = false;
    switch (xenos::PrimitiveType(state.primitive)) {
      case xenos::PrimitiveType::kTriangleList: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
      case xenos::PrimitiveType::kTriangleStrip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
      case xenos::PrimitiveType::kTriangleFan: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
      case xenos::PrimitiveType::kQuadList:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        quads = true;
        break;
      default:
        ++skipped_primitive_;
        return;
    }
    const AlphaTest alpha_test = GetAlphaTest(state);
    if (alpha_test == AlphaTest::kRejectAll) return;
    RenderTarget* target = GetTarget(state.surface_info, state.color_info);
    if (!target) return;
    ApplyPendingClear(target);
    const Pipeline& pipeline = GetPipeline(state, topology, target->format, target->samples);
    if (pipeline.pipeline == VK_NULL_HANDLE) {
      ++skipped_pipeline_;
      return;
    }
    const auto& dfn = device_->functions();

    // Vertex streams: big-endian 32-bit words to little-endian. Unused
    // bindings point at offset 0 (the pipeline does not read them).
    std::array<VkBuffer, 4> vertex_buffers;
    vertex_buffers.fill(ring_buffer_);
    std::array<VkDeviceSize, 4> vertex_offsets{};
    for (uint32_t s = 0; s < 4; ++s) {
      const auto& stream = state.streams[s];
      if (!stream.size()) continue;
      if (stream.address) frame_streams_[stream.address] = true;
      // Guest vertex buffers come from the stream cache when their contents
      // are unchanged (section 20.15); the rest go through the ring.
      if (stream.guest && UseCachedStream(pipeline, s, stream, vertex_buffers[s],
                                          vertex_offsets[s])) {
        continue;
      }
      const VkDeviceSize offset = Allocate(stream.size(), 16);
      if (offset == VK_WHOLE_SIZE) return;
      vertex_bytes_ += stream.size();
      ConvertStream(pipeline, s, stream, ring_mapping_ + offset);
      vertex_offsets[s] = offset;
    }
    // Constants for set 0 (dynamic offsets).
    std::array<uint32_t, 3> dynamic_offsets{};
    const VkDeviceSize vs_offset = Allocate(kConstantsRange, 256);
    const VkDeviceSize ps_offset = Allocate(kConstantsRange, 256);
    const VkDeviceSize shared_offset = Allocate(kSharedConstantsRange, 256);
    if (vs_offset == VK_WHOLE_SIZE || ps_offset == VK_WHOLE_SIZE ||
        shared_offset == VK_WHOLE_SIZE) {
      return;
    }
    std::memcpy(ring_mapping_ + vs_offset, state.vertex_constants.data(), kConstantsRange);
    std::memcpy(ring_mapping_ + ps_offset, state.pixel_constants.data(), kConstantsRange);
    std::memset(ring_mapping_ + shared_offset, 0, kSharedConstantsRange);
    std::memcpy(ring_mapping_ + shared_offset + kAlphaThresholdOffset, &state.alpha_ref,
                sizeof(state.alpha_ref));
    // Booleans: XenosRecomp tests bit n for vertex shader bn and bit 16 + n
    // for pixel shader bn. Loops: i{loop id} is int4(count, start, step).
    const uint32_t booleans = (state.vertex_bools & 0xFFFF) | (state.pixel_bools << 16);
    std::memcpy(ring_mapping_ + shared_offset + kBooleansOffset, &booleans, sizeof(booleans));
    // oPos.xy = oPos.xy * g_PositionScale + g_HalfPixelOffset * oPos.w. With
    // the viewport transform off (PA_CL_VTE_CNTL bits 0-3 clear) positions are
    // pixels of the target; map them to clip space for a full-target viewport
    // (y is negated afterwards by -fvk-invert-y).
    std::array<float, 2> position_scale{1, 1}, position_offset{0, 0};
    if ((state.vte_control & 0xF) == 0) {
      position_scale = {2.0f / float(target->width), -2.0f / float(target->height)};
      position_offset = {-1, 1};
    }
    std::memcpy(ring_mapping_ + shared_offset + kHalfPixelOffsetOffset, position_offset.data(),
                sizeof(position_offset));
    std::memcpy(ring_mapping_ + shared_offset + kPositionScaleOffset, position_scale.data(),
                sizeof(position_scale));
    // The pixel shader multiplies oC0 by this after the alpha test.
    const float output_scale = OutputScale(state.color_info);
    std::memcpy(ring_mapping_ + shared_offset + kOutputScaleOffset, &output_scale,
                sizeof(output_scale));
    for (uint32_t i = 0; i < 32; ++i) {
      const uint32_t loop = state.loop_constants[i];
      const std::array<int32_t, 4> value{int32_t(loop & 0xFF), int32_t((loop >> 8) & 0xFF),
                                         int32_t(int8_t(loop >> 16)), 0};
      std::memcpy(ring_mapping_ + shared_offset + kLoopConstantsOffset + i * 16, value.data(),
                  sizeof(value));
    }
    dynamic_offsets = {uint32_t(vs_offset), uint32_t(ps_offset), uint32_t(shared_offset)};

    VkDescriptorSet texture_set = WriteTextureSet(state);
    if (texture_set == VK_NULL_HANDLE) return;

    if (Dumping()) {
      REXGPU_INFO("[native] draw {}: target {:08X}/{:08X} vs {:016X} ps {:016X} prim {} count {} "
                  "vport {} {} {} {} {} {} vte {:08X} depth {:08X} blend {:08X} mask {:X}",
                  drawn_, state.surface_info, state.color_info, state.vertex_shader,
                  state.pixel_shader, state.primitive, state.vertex_count, state.viewport[0],
                  state.viewport[1], state.viewport[2], state.viewport[3], state.viewport[4],
                  state.viewport[5], state.vte_control, state.depth_control, state.blend_control,
                  state.color_mask & 0xF);
      std::string loops;
      for (uint32_t i = 0; i < 32; ++i) {
        if (state.loop_constants[i]) loops += fmt::format(" i{}={:08X}", i, state.loop_constants[i]);
      }
      REXGPU_INFO("[native] draw {}: bools {:08X} {:08X} loops{}", drawn_, state.vertex_bools,
                  state.pixel_bools, loops);
      if ((state.vte_control & 0xF) == 0) {
        // Screen-space draws: the first vertices of stream 0 (big-endian
        // floats) and texture 0's base address.
        const auto& stream = state.streams[0];
        std::string values;
        for (size_t i = 0; i + 4 <= stream.size() && i < 4 * stream.stride; i += 4) {
          uint32_t word;
          std::memcpy(&word, stream.bytes() + i, 4);
          word = __builtin_bswap32(word);
          float value;
          std::memcpy(&value, &word, 4);
          values += fmt::format(" {}", value);
        }
        REXGPU_INFO("[native] draw {}: stride {} vertices{} tex0 {:08X}", drawn_,
                    state.streams[0].stride, values, state.texture_fetch[0][1] & 0xFFFFF000);
      }
    }
    BindTarget(target);
    SetDrawViewport(state, *target);
    dfn.vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
    std::array<VkDescriptorSet, 2> sets{constants_set_, texture_set};
    dfn.vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, uint32_t(sets.size()), sets.data(),
                                uint32_t(dynamic_offsets.size()), dynamic_offsets.data());
    dfn.vkCmdBindVertexBuffers(command_buffer_, 0, uint32_t(vertex_buffers.size()),
                               vertex_buffers.data(), vertex_offsets.data());
    const bool indexed = !state.indices.empty();
    if (quads || indexed) {
      // Index n of the draw: the game's index (indexed) or n itself.
      auto source_index = [&](uint32_t n) { return indexed ? state.indices[n] : n; };
      const uint32_t quad_count = state.vertex_count / 4;
      const uint32_t index_count = quads ? quad_count * 6 : state.vertex_count;
      const VkDeviceSize index_offset = Allocate(index_count * sizeof(uint32_t), 4);
      if (index_offset == VK_WHOLE_SIZE) return;
      auto* indices = reinterpret_cast<uint32_t*>(ring_mapping_ + index_offset);
      if (quads) {
        for (uint32_t q = 0; q < quad_count; ++q) {
          const uint32_t v = q * 4;
          const uint32_t pattern[6] = {source_index(v), source_index(v + 1), source_index(v + 2),
                                       source_index(v), source_index(v + 2), source_index(v + 3)};
          std::memcpy(indices + q * 6, pattern, sizeof(pattern));
        }
      } else {
        std::memcpy(indices, state.indices.data(), index_count * sizeof(uint32_t));
      }
      dfn.vkCmdBindIndexBuffer(command_buffer_, ring_buffer_, index_offset, VK_INDEX_TYPE_UINT32);
      dfn.vkCmdDrawIndexed(command_buffer_, index_count, 1, 0, state.base_vertex, 0);
    } else {
      dfn.vkCmdDraw(command_buffer_, state.vertex_count, 1, 0, 0);
    }
    ++drawn_;
  }

  // Called on the guest thread at every Present.
  void Present(uint64_t frame) {
    BeginFrameIfNeeded();
    const auto& dfn = device_->functions();
    // The back buffer must be in SHADER_READ_ONLY_OPTIMAL with this frame's
    // contents (a clear if nothing was drawn to it).
    if (!back_buffer_cleared_) BindTarget(back_buffer_target_);
    EndPass();
    DumpImage(back_buffer_.image, VK_IMAGE_ASPECT_COLOR_BIT, kBackBufferWidth, kBackBufferHeight,
              false, "back-buffer");
    bool submitted = false;
    presenter_->RefreshGuestOutput(
        kBackBufferWidth, kBackBufferHeight, kBackBufferWidth, kBackBufferHeight,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& vk_context =
              static_cast<vk_ui::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
          VkFramebuffer framebuffer = GetOutputFramebuffer(vk_context.image_view());
          if (framebuffer == VK_NULL_HANDLE) return false;
          VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
          barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = vk_context.image();
          barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          dfn.vkCmdPipelineBarrier(command_buffer_,
                                   vk_context.image_ever_written_previously()
                                       ? vk_ui::VulkanPresenter::kGuestOutputInternalStageMask
                                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                                   0, nullptr, 1, &barrier);
          VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
          pass.renderPass = blit_render_pass_;
          pass.framebuffer = framebuffer;
          pass.renderArea = {{0, 0}, {kBackBufferWidth, kBackBufferHeight}};
          dfn.vkCmdBeginRenderPass(command_buffer_, &pass, VK_SUBPASS_CONTENTS_INLINE);
          const VkDeviceSize gamma_offset = Allocate(kGammaRampSize, 256);
          if (gamma_offset == VK_WHOLE_SIZE) return false;
          WriteGammaRamp(reinterpret_cast<uint32_t*>(ring_mapping_ + gamma_offset));
          const uint32_t dynamic_offset = uint32_t(gamma_offset);
          SetFullViewport();
          dfn.vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, blit_pipeline_);
          dfn.vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      blit_layout_, 0, 1, &blit_set_, 1, &dynamic_offset);
          dfn.vkCmdDraw(command_buffer_, 3, 1, 0, 0);
          dfn.vkCmdEndRenderPass(command_buffer_);
          submitted = SubmitFrame();
          return submitted;
        });
    if (!submitted) {
      // The presenter did not call back (for example while minimised).
      SubmitFrame();
    }
    frame_open_ = false;
    WriteDumps(frame);
    recording_frame_ = frame + 1;
    // Every 600 frames, or every 5 seconds when frames are slow.
    const auto now = std::chrono::steady_clock::now();
    if (frame % 600 == 0 || now - last_report_ > std::chrono::seconds(5)) {
      last_report_ = now;
      REXGPU_INFO("[native] frame {}: {} draws, {} drawn, skipped {} (primitive) {} (pipeline), "
                  "{} pipelines, {} textures ({} loaded with mips), {} shaders missing, "
                  "{} render targets, {} resolves",
                  frame, frame_draws_, drawn_, skipped_primitive_, skipped_pipeline_,
                  pipelines_.size(), textures_.size(), mipped_textures_,
                  missing_shaders_.size(), render_targets_.size(), resolves_);
      REXGPU_INFO("[native] frame {} uploads: {} KB of vertices converted, {} KB cached, from {} "
                  "buffers, ring {} KB, {} ring overflows, {} descriptor overflows; stream cache "
                  "{} KB in {} ranges, {} flushes",
                  frame, vertex_bytes_ / 1024, cached_bytes_ / 1024, frame_streams_.size(),
                  ring_offset_ / 1024, ring_overflows_, descriptor_overflows_,
                  stream_cache_offset_ / 1024, stream_cache_.size(), stream_cache_flushes_);
      auto ms = [](std::chrono::steady_clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
      };
      REXGPU_INFO("[native] frame {} times: draws {:.1f} ms (texture creation {:.1f} ms, {} "
                  "textures), fence wait {:.1f} ms, frame {:.1f} ms",
                  frame, ms(draw_time_), ms(texture_create_time_), textures_created_,
                  ms(fence_wait_time_), ms(now - frame_start_));
      if (report_frames_) {
        REXGPU_INFO("[native] frame {} average over {} frames: draws {:.2f} ms", frame,
                    report_frames_, ms(report_draw_time_) / report_frames_);
      }
      report_draw_time_ = {};
      report_frames_ = 0;
    }
    report_draw_time_ += draw_time_;
    ++report_frames_;
    frame_start_ = now;
    draw_time_ = texture_create_time_ = fence_wait_time_ = {};
    textures_created_ = 0;
    frame_draws_ = drawn_ = skipped_primitive_ = skipped_pipeline_ = resolves_ = 0;
    vertex_bytes_ = cached_bytes_ = ring_overflows_ = descriptor_overflows_ = 0;
    frame_streams_.clear();
  }

 private:
  // --- Frame -----------------------------------------------------------------

  void BeginFrameIfNeeded() {
    if (frame_open_) return;
    const auto& dfn = device_->functions();
    const VkDevice device = device_->device();
    const auto wait_start = std::chrono::steady_clock::now();
    dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
    fence_wait_time_ += std::chrono::steady_clock::now() - wait_start;
    FreeRetired();  // the previous frame, which could use them, has finished
    dfn.vkResetFences(device, 1, &fence_);
    dfn.vkResetCommandPool(device, command_pool_, 0);
    dfn.vkResetDescriptorPool(device, frame_descriptor_pool_, 0);
    ring_offset_ = 0;
    if (stream_cache_flush_) {
      // The previous frame, the last reader of the cache, has finished.
      stream_cache_.clear();
      stream_cache_offset_ = 0;
      stream_cache_flush_ = false;
      ++stream_cache_flushes_;
    }
    frame_textures_.clear();
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn.vkBeginCommandBuffer(command_buffer_, &begin);
    // Render passes begin on demand, per render target (BindTarget).
    current_target_ = nullptr;
    back_buffer_cleared_ = false;
    frame_open_ = true;
  }

  // --- Render targets (section 20.5) -----------------------------------------
  //
  // Each EDRAM color surface (RB_COLOR_INFO base and format, RB_SURFACE_INFO
  // pitch) becomes a Vulkan image, kept in SHADER_READ_ONLY_OPTIMAL between
  // render passes. The back buffer (base 0, pitch 1280) is back_buffer_.

  struct RenderTarget {
    Texture image;
    Texture depth;  // kDepthFormat, always DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkFormat format = kBackBufferFormat;
    uint32_t width = 0, height = 0;
    bool initialized = false;
    bool is_back_buffer = false;
    bool fixed16 = false;  // k_16_16 or k_16_16_16_16, held as half floats
    // An anti-aliased surface is drawn in tiles that share one EDRAM range,
    // so a resolve's clear covers the whole screen. It waits until the
    // target's next draw or clear: later tiles still resolve their part
    // first (section 20.18).
    bool tiled = false;
    ClearValues pending_clear;
    // Anti-aliased targets draw into multisampled images, resolved into image
    // and depth at the end of every render pass (section 20.12).
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    Texture msaa_image, msaa_depth;
  };

  // Render passes for one color format: the first pass on a target clears
  // it, later ones load it. Pipelines are created against the clear pass
  // (the two are compatible).
  struct TargetPasses {
    VkRenderPass clear = VK_NULL_HANDLE, load = VK_NULL_HANDLE;
  };

  const TargetPasses* GetPasses(VkFormat format,
                                VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
    const uint64_t key = uint64_t(format) | (uint64_t(samples) << 32);
    auto it = target_passes_.find(key);
    if (it != target_passes_.end()) return &it->second;
    TargetPasses passes;
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
      passes.clear = CreateMsaaRenderPass(format, samples, VK_ATTACHMENT_LOAD_OP_CLEAR);
      passes.load = CreateMsaaRenderPass(format, samples, VK_ATTACHMENT_LOAD_OP_LOAD);
    } else {
      passes.clear = CreateRenderPass(format, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                      VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                      VK_ACCESS_SHADER_READ_BIT, true);
      passes.load = CreateRenderPass(format, VK_ATTACHMENT_LOAD_OP_LOAD,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_ACCESS_SHADER_READ_BIT, true);
    }
    if (!passes.clear || !passes.load) return nullptr;
    return &target_passes_.emplace(key, passes).first->second;
  }

  struct ResolvedTexture {
    Texture texture;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t swizzle = 0;
    bool initialized = false;
  };

  static uint32_t TargetKey(uint32_t surface_info, uint32_t color_info) {
    reg::RB_SURFACE_INFO surface;
    surface.value = surface_info;
    reg::RB_COLOR_INFO color;
    color.value = color_info;
    // color_base and color_base_bit_11: the 12-bit EDRAM tile index. The
    // sample count is part of the key: the 4x scene target 05020140 has the
    // same pitch and tile as the 1x bloom target 05000140 (section 20.17).
    return (color_info & 0xFFF) | (surface.surface_pitch << 12) |
           (uint32_t(color.color_format) << 26) | (uint32_t(surface.msaa_samples) << 30);
  }

  RenderTarget* GetTarget(uint32_t surface_info, uint32_t color_info) {
    const uint32_t key = TargetKey(surface_info, color_info);
    auto it = render_targets_.find(key);
    if (it != render_targets_.end()) return &it->second;
    reg::RB_SURFACE_INFO surface;
    surface.value = surface_info;
    RenderTarget target;
    // EDRAM surfaces have no height; every target gets the largest one used.
    target.width = std::clamp<uint32_t>(surface.surface_pitch, 1, kMaxTargetWidth);
    target.height = kMaxTargetHeight;
    // An anti-aliased surface is one tile of a predicated-tiling pass: the
    // GPU replays the frame's draws once per tile, but the game issues each
    // draw once with the whole screen's viewport, and resolves each tile's
    // rect. Here the target holds the whole screen, drawn once with the
    // surface's sample count, and each tile's resolve copies its part
    // (sections 20.10 and 20.12).
    if (surface.msaa_samples != xenos::MsaaSamples::k1X) {
      target.tiled = true;
      target.width = std::max(target.width, kBackBufferWidth);
      if (REXCVAR_GET(wwe2k15_native_msaa) && create_render_pass2_) {
        target.samples = surface.msaa_samples == xenos::MsaaSamples::k2X ? VK_SAMPLE_COUNT_2_BIT
                                                                         : VK_SAMPLE_COUNT_4_BIT;
      }
    }
    reg::RB_COLOR_INFO color;
    color.value = color_info;
    target.format = TargetFormat(color.color_format);
    target.fixed16 = IsFixed16(color.color_format);
    target.image = CreateTargetImage(target.width, target.height,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                     target.format);
    if (target.image.view == VK_NULL_HANDLE) return nullptr;
    target.depth = CreateDepthImage(target.width, target.height);
    if (target.depth.view == VK_NULL_HANDLE) return nullptr;
    if (target.samples != VK_SAMPLE_COUNT_1_BIT) {
      target.msaa_image = CreateTargetImage(target.width, target.height,
                                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, target.format,
                                            {}, target.samples);
      target.msaa_depth = CreateDepthImage(target.width, target.height, target.samples);
      if (target.msaa_image.view == VK_NULL_HANDLE || target.msaa_depth.view == VK_NULL_HANDLE) {
        return nullptr;
      }
    }
    target.framebuffer = CreateFramebuffer(target);
    if (target.framebuffer == VK_NULL_HANDLE) return nullptr;
    REXGPU_INFO("[native] new render target: key {:08X} (surface_info {:08X} color_info {:08X}) "
                "{}x{} format {} samples {}", key, surface_info, color_info, target.width,
                target.height, uint32_t(target.format), uint32_t(target.samples));
    return &render_targets_.emplace(key, target).first->second;
  }

  // Makes target the current render pass, ending the previous one.
  void BindTarget(RenderTarget* target) {
    if (target == current_target_) return;
    EndPass();
    const auto& dfn = device_->functions();
    // A target's first pass (and the back buffer's first pass each frame)
    // clears it; later passes keep its contents.
    const bool clear = target->is_back_buffer ? !back_buffer_cleared_ : !target->initialized;
    // Depth 0 is the far plane: the game's depth range is reversed
    // (PA_CL_VPORT_ZSCALE -1, ZOFFSET 1, tests GREATER_EQUAL).
    std::array<VkClearValue, 2> clear_values{};
    clear_values[0].color.float32[3] = target->is_back_buffer ? 1.0f : 0.0f;
    clear_values[1].depthStencil = {0.0f, 0};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    const TargetPasses* passes = GetPasses(target->format, target->samples);
    pass.renderPass = clear ? passes->clear : passes->load;
    pass.framebuffer = target->framebuffer;
    pass.renderArea = {{0, 0}, {target->width, target->height}};
    pass.clearValueCount = uint32_t(clear_values.size());
    pass.pClearValues = clear_values.data();
    dfn.vkCmdBeginRenderPass(command_buffer_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    target->initialized = true;
    if (target->is_back_buffer) back_buffer_cleared_ = true;
    current_target_ = target;
  }

  void EndPass() {
    if (!current_target_) return;
    device_->functions().vkCmdEndRenderPass(command_buffer_);
    current_target_ = nullptr;
  }

  // Viewport from PA_CL_VPORT_* when the VTE scales and offsets x and y
  // (PA_CL_VTE_CNTL bits 0-3); otherwise the whole target. Depth: z * ZSCALE
  // + ZOFFSET when bits 4-5 are set (Vulkan allows minDepth > maxDepth, which
  // the game's reversed range needs).
  void SetDrawViewport(const NativeDrawState& state, const RenderTarget& target) {
    const auto& dfn = device_->functions();
    VkViewport viewport{0, 0, float(target.width), float(target.height), 0, 1};
    if ((state.vte_control & 0xF) == 0xF && state.viewport[0] != 0 && state.viewport[2] != 0) {
      const float half_width = std::abs(state.viewport[0]);
      const float half_height = std::abs(state.viewport[2]);
      viewport.x = state.viewport[1] - half_width;
      viewport.y = state.viewport[3] - half_height;
      viewport.width = half_width * 2;
      viewport.height = half_height * 2;
    }
    if ((state.vte_control & 0x30) == 0x30) {
      viewport.minDepth = std::clamp(state.viewport[5], 0.0f, 1.0f);
      viewport.maxDepth = std::clamp(state.viewport[5] + state.viewport[4], 0.0f, 1.0f);
    }
    VkRect2D scissor{{0, 0}, {target.width, target.height}};
    dfn.vkCmdSetViewport(command_buffer_, 0, 1, &viewport);
    dfn.vkCmdSetScissor(command_buffer_, 0, 1, &scissor);
  }

 public:
  void Clear(uint32_t surface_info, uint32_t color_info, const ClearValues& clear,
             const std::vector<GuestRect>& rects) {
    BeginFrameIfNeeded();
    RenderTarget* target = GetTarget(surface_info, color_info);
    if (!target) return;
    std::vector<VkClearRect> clear_rects;
    for (const GuestRect& rect : rects) {
      VkRect2D area;
      if (ClampRect(rect, target->width, target->height, area)) {
        clear_rects.push_back({area, 0, 1});
      }
    }
    if (rects.empty()) clear_rects.push_back({{{0, 0}, {target->width, target->height}}, 0, 1});
    ApplyPendingClear(target);
    if (Dumping()) {
      REXGPU_INFO("[native] clear after draw {}: target {:08X}/{:08X} color {} depth {} z {} "
                  "rects {}",
                  drawn_, surface_info, color_info, clear.color, clear.depth, clear.z,
                  rects.size());
    }
    ClearRects(target, clear, clear_rects);
  }

 private:
  // Clears the given parts of target's color and depth buffers.
  void ClearRects(RenderTarget* target, const ClearValues& clear,
                  const std::vector<VkClearRect>& rects) {
    if (rects.empty() || !(clear.color || clear.depth || clear.stencil)) return;
    BindTarget(target);
    std::array<VkClearAttachment, 2> attachments{};
    uint32_t count = 0;
    if (clear.color) {
      VkClearAttachment& attachment = attachments[count++];
      attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      for (uint32_t i = 0; i < 4; ++i) attachment.clearValue.color.float32[i] = clear.rgba[i];
    }
    if (clear.depth || clear.stencil) {
      VkClearAttachment& attachment = attachments[count++];
      attachment.aspectMask = (clear.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                              (clear.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
      attachment.clearValue.depthStencil = {std::clamp(clear.z, 0.0f, 1.0f),
                                            clear.stencil_value};
    }
    device_->functions().vkCmdClearAttachments(command_buffer_, count, attachments.data(),
                                               uint32_t(rects.size()), rects.data());
  }

  // Clears all of a tiled target with the clear its last resolve left.
  void ApplyPendingClear(RenderTarget* target) {
    const ClearValues clear = target->pending_clear;
    if (!(clear.color || clear.depth || clear.stencil)) return;
    target->pending_clear = {};
    ClearRects(target, clear, {{{{0, 0}, {target->width, target->height}}, 0, 1}});
  }

 public:

  // Copies a render target into the texture with the destination's fetch
  // constant (keyed by its base address), as the GPU's EDRAM resolve does.
  void Resolve(uint32_t surface_info, uint32_t color_info, bool depth,
               const std::array<uint32_t, 6>& dest_words, bool has_rect, GuestRect rect,
               const ClearValues& clear) {
    BeginFrameIfNeeded();
    RenderTarget* source = GetTarget(surface_info, color_info);
    if (!source) return;
    xenos::xe_gpu_texture_fetch_t dest;
    std::memcpy(&dest, dest_words.data(), sizeof(dest));
    if (dest.dimension != xenos::DataDimension::k2DOrStacked || !dest.base_address) return;
    const uint32_t width = dest.size_2d.width + 1, height = dest.size_2d.height + 1;
    if (!has_rect) rect = {0, 0, int32_t(width), int32_t(height)};
    VkRect2D area;
    if (!ClampRect(rect, std::min(source->width, width), std::min(source->height, height),
                   area)) {
      return;
    }
    if (Dumping()) {
      REXGPU_INFO("[native] resolve after draw {} (dump {}): target {:08X}/{:08X} depth {} "
                  "rect {} {} {} {} to {:08X}",
                  drawn_, pending_dumps_.size(), surface_info, color_info, depth, rect[0], rect[1],
                  rect[2], rect[3], dest.base_address << 12);
    }
    if (source->initialized) {
      if (depth) {
        CopyDepth(*source, dest.base_address, width, height, area);
      } else {
        CopyColor(*source, dest, width, height, area);
      }
    }
    // D3DRESOLVE_CLEAR*: the resolved part of the target is cleared after the
    // copy (EDRAM tiles; here the same rect of the whole-screen target). A
    // tiled target is cleared whole, before its next draw (section 20.18).
    if (source->tiled) {
      if (clear.color || clear.depth || clear.stencil) source->pending_clear = clear;
      return;
    }
    VkRect2D clear_area;
    if (ClampRect(rect, source->width, source->height, clear_area)) {
      ClearRects(source, clear, {{clear_area, 0, 1}});
    }
  }

 private:
  static constexpr uint32_t kIdentitySwizzle = 0x688;  // XYZW

  // The resolve destination at base_address with this size, format and
  // view swizzle (the destination texture's fetch swizzle), recreated when
  // any of them changes.
  ResolvedTexture* GetResolved(uint32_t base_address, uint32_t width, uint32_t height,
                               VkFormat format, uint32_t swizzle = kIdentitySwizzle) {
    ResolvedTexture& resolved = resolved_[PhysicalPage(base_address)];
    if (resolved.width != width || resolved.height != height || resolved.format != format ||
        resolved.swizzle != swizzle) {
      Retire(resolved.texture);
      resolved = ResolvedTexture{};
      VkComponentMapping components{};
      for (uint32_t i = 0; i < 4; ++i) {
        (&components.r)[i] = ViewSwizzle(swizzle, i, false);
      }
      resolved.texture = CreateTargetImage(
          width, height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, format,
          components);
      if (resolved.texture.view == VK_NULL_HANDLE) return nullptr;
      resolved.width = width;
      resolved.height = height;
      resolved.format = format;
      resolved.swizzle = swizzle;
    }
    return &resolved;
  }

  // Depth resolve: target's depth (float, already in the game's reversed
  // range) into an R32_SFLOAT texture, which the game's shaders read in x.
  // Depth and color images cannot be copied into each other directly, so the
  // copy goes through depth_copy_buffer_.
  void CopyDepth(RenderTarget& source, uint32_t base_address, uint32_t width, uint32_t height,
                 const VkRect2D& area) {
    ResolvedTexture* resolved = GetResolved(base_address, width, height, kDepthTextureFormat);
    if (!resolved) return;
    EndPass();
    const auto& dfn = device_->functions();
    std::array<VkImageMemoryBarrier, 2> barriers{};
    for (auto& barrier : barriers) {
      barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    barriers[0].image = source.depth.image;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1,
                                    0, 1};
    barriers[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[1].image = resolved->texture.image;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = resolved->initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                  : VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dfn.vkCmdPipelineBarrier(command_buffer_,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             uint32_t(barriers.size()), barriers.data());
    // Tightly packed rows of the rect in the buffer.
    VkBufferImageCopy copy{};
    copy.bufferRowLength = area.extent.width;
    copy.bufferImageHeight = area.extent.height;
    copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.imageOffset = {area.offset.x, area.offset.y, 0};
    copy.imageExtent = {area.extent.width, area.extent.height, 1};
    dfn.vkCmdCopyImageToBuffer(command_buffer_, source.depth.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_copy_buffer_, 1, &copy);
    VkBufferMemoryBarrier buffer_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    buffer_barrier.srcQueueFamilyIndex = buffer_barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.buffer = depth_copy_buffer_;
    buffer_barrier.size = VK_WHOLE_SIZE;
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &buffer_barrier, 0,
                             nullptr);
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dfn.vkCmdCopyBufferToImage(command_buffer_, depth_copy_buffer_, resolved->texture.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    // Back: the depth buffer for drawing (and the next copy's buffer
    // writes after this one's reads), the texture for sampling.
    buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 1, &buffer_barrier, uint32_t(barriers.size()),
                             barriers.data());
    resolved->initialized = true;
    ++resolves_;
    DumpImage(resolved->texture.image, VK_IMAGE_ASPECT_COLOR_BIT, width, height, true,
              fmt::format("{:08X}-depth", base_address << 12));
  }

  // --- Frame dumps (--wwe2k15_native_dump_resolves) ---------------------------

  struct PendingDump {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    bool depth = false;  // R32_SFLOAT, shown as grey
    VkFormat format = kBackBufferFormat;
    std::string name;
  };

  static float HalfToFloat(uint16_t half) {
    const uint32_t sign = uint32_t(half >> 15) << 31;
    const uint32_t exponent = (half >> 10) & 0x1F, mantissa = half & 0x3FF;
    if (exponent == 0) {
      const float value = std::ldexp(float(mantissa), -24);
      return sign ? -value : value;
    }
    uint32_t bits = sign | ((exponent == 31 ? 255 : exponent + 112) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  }

  // One texel of a dumped color image as 8-bit RGB (float formats clamped to
  // [0, 1]).
  static void DumpTexel(VkFormat format, const uint8_t* in, uint8_t rgb[3]) {
    auto to8 = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    switch (format) {
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
        uint32_t v;
        std::memcpy(&v, in, 4);
        for (int i = 0; i < 3; ++i) rgb[i] = uint8_t(((v >> (10 * i)) & 0x3FF) >> 2);
        break;
      }
      case VK_FORMAT_R16G16B16A16_UNORM:
      case VK_FORMAT_R16G16_UNORM:
      case VK_FORMAT_R16G16B16A16_SNORM:
      case VK_FORMAT_R16G16_SNORM: {
        const bool two = format == VK_FORMAT_R16G16_UNORM || format == VK_FORMAT_R16G16_SNORM;
        const bool snorm =
            format == VK_FORMAT_R16G16_SNORM || format == VK_FORMAT_R16G16B16A16_SNORM;
        for (int i = 0; i < 3; ++i) {
          uint16_t v = 0;
          if (i < (two ? 2 : 3)) std::memcpy(&v, in + i * 2, 2);
          rgb[i] = to8(snorm ? float(int16_t(v)) / 32767.0f : float(v) / 65535.0f);
        }
        break;
      }
      case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_R16G16_SFLOAT: {
        const int channels = format == VK_FORMAT_R16G16_SFLOAT ? 2 : 3;
        for (int i = 0; i < 3; ++i) {
          uint16_t h = 0;
          if (i < channels) std::memcpy(&h, in + i * 2, 2);
          rgb[i] = to8(HalfToFloat(h));
        }
        break;
      }
      case VK_FORMAT_R32_SFLOAT:
      case VK_FORMAT_R32G32_SFLOAT: {
        const int channels = format == VK_FORMAT_R32_SFLOAT ? 1 : 2;
        for (int i = 0; i < 3; ++i) {
          float f = 0;
          if (i < channels) std::memcpy(&f, in + i * 4, 4);
          rgb[i] = to8(f);
        }
        break;
      }
      default:
        rgb[0] = in[0];
        rgb[1] = in[1];
        rgb[2] = in[2];
    }
  }

  bool Dumping() const {
    const int32_t frame = REXCVAR_GET(wwe2k15_native_dump_resolves);
    return frame > 0 && recording_frame_ == uint64_t(frame);
  }

  // Copies image (in SHADER_READ_ONLY_OPTIMAL, 4 bytes per pixel) into a
  // readback buffer; Present writes the file once the frame has finished.
  void DumpImage(VkImage image, VkImageAspectFlags aspect, uint32_t width, uint32_t height,
                 bool depth, const std::string& name, VkFormat format = kBackBufferFormat,
                 bool force = false) {
    if (!Dumping() && !force) return;
    PendingDump dump;
    dump.format = depth ? kDepthTextureFormat : format;
    if (!vk_ui::util::CreateDedicatedAllocationBuffer(
            device_, VkDeviceSize(width) * height * FormatTexelSize(dump.format),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            vk_ui::util::MemoryPurpose::kReadback, dump.buffer, dump.memory)) {
      REXGPU_WARN("[native] dump {}: no readback buffer for {}x{}", name, width, height);
      return;
    }
    dump.width = width;
    dump.height = height;
    dump.depth = depth;
    dump.name = fmt::format("{:03}-{}-{}x{}", pending_dumps_.size(), name, width, height);
    EndPass();
    const auto& dfn = device_->functions();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {aspect, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    dfn.vkCmdCopyImageToBuffer(command_buffer_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dump.buffer, 1, &copy);
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    pending_dumps_.push_back(std::move(dump));
  }

  // After the frame's fence: each dump as a 24-bit BMP (depth as grey,
  // stretched between its smallest and largest value).
  void WriteDumps(uint64_t frame) {
    if (pending_dumps_.empty()) return;
    const auto& dfn = device_->functions();
    const VkDevice device = device_->device();
    dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
    const std::filesystem::path dir =
        std::filesystem::absolute(fmt::format("native-dumps/frame-{}", frame));
    std::filesystem::create_directories(dir);
    for (PendingDump& dump : pending_dumps_) {
      void* mapping = nullptr;
      if (dfn.vkMapMemory(device, dump.memory, 0, VK_WHOLE_SIZE, 0, &mapping) == VK_SUCCESS) {
        const auto* pixels = static_cast<const uint8_t*>(mapping);
        float low = 1.0f, high = 0.0f;
        if (dump.depth) {
          for (uint32_t i = 0; i < dump.width * dump.height; ++i) {
            float z;
            std::memcpy(&z, pixels + i * 4, 4);
            low = std::min(low, z);
            high = std::max(high, z);
          }
        }
        const uint32_t row = (dump.width * 3 + 3) & ~3u;
        std::vector<uint8_t> bmp(54 + size_t(row) * dump.height);
        auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&bmp[at], &v, 4); };
        bmp[0] = 'B';
        bmp[1] = 'M';
        put32(2, uint32_t(bmp.size()));
        put32(10, 54);
        put32(14, 40);
        put32(18, dump.width);
        put32(22, dump.height);
        bmp[26] = 1;
        bmp[28] = 24;
        for (uint32_t y = 0; y < dump.height; ++y) {
          uint8_t* out = &bmp[54 + size_t(dump.height - 1 - y) * row];  // bottom-up
          for (uint32_t x = 0; x < dump.width; ++x) {
            const uint8_t* in =
                pixels + (size_t(y) * dump.width + x) * FormatTexelSize(dump.format);
            if (dump.depth) {
              float z;
              std::memcpy(&z, in, 4);
              const float t = high > low ? (z - low) / (high - low) : 0.0f;
              out[x * 3] = out[x * 3 + 1] = out[x * 3 + 2] = uint8_t(t * 255.0f + 0.5f);
            } else {
              uint8_t rgb[3];
              DumpTexel(dump.format, in, rgb);
              out[x * 3] = rgb[2];
              out[x * 3 + 1] = rgb[1];
              out[x * 3 + 2] = rgb[0];
            }
          }
        }
        std::ofstream(dir / (dump.name + ".bmp"), std::ios::binary)
            .write(reinterpret_cast<const char*>(bmp.data()), std::streamsize(bmp.size()));
        if (dump.depth) {
          REXGPU_INFO("[native] dump {}: depth {} to {}", dump.name, low, high);
        }
        dfn.vkUnmapMemory(device, dump.memory);
      }
      dfn.vkDestroyBuffer(device, dump.buffer, nullptr);
      dfn.vkFreeMemory(device, dump.memory, nullptr);
    }
    REXGPU_INFO("[native] wrote {} images of frame {} to {}", pending_dumps_.size(), frame,
                dir.string());
    pending_dumps_.clear();
  }

  void CopyColor(RenderTarget& source_target, const xenos::xe_gpu_texture_fetch_t& dest,
                 uint32_t width, uint32_t height, const VkRect2D& area) {
    RenderTarget* source = &source_target;
    const uint32_t base_address = dest.base_address;
    // The texture is read through this image with the destination's
    // swizzle, whatever format its fetch constant names (section 20.11).
    const VkFormat format = ResolvedFormat(source->format, source->fixed16);
    ResolvedTexture* resolved_texture =
        GetResolved(base_address, width, height, format, ResolveSwizzle(dest.swizzle));
    if (!resolved_texture) return;
    ResolvedTexture& resolved = *resolved_texture;
    EndPass();
    const auto& dfn = device_->functions();
    std::array<VkImageMemoryBarrier, 2> barriers{};
    for (auto& barrier : barriers) {
      barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    barriers[0].image = source->image.image;
    barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[1].image = resolved.texture.image;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = resolved.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dfn.vkCmdPipelineBarrier(command_buffer_,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             uint32_t(barriers.size()), barriers.data());
    // The destination point is the source rect's corner (no pDestPoint).
    if (format != source->format) {
      // Converting blit, same size: float to UNORM clamps to 0...1.
      VkImageBlit blit{};
      blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[0] = blit.dstOffsets[0] = {area.offset.x, area.offset.y, 0};
      blit.srcOffsets[1] = blit.dstOffsets[1] = {
          area.offset.x + int32_t(area.extent.width), area.offset.y + int32_t(area.extent.height),
          1};
      cmd_blit_image_(command_buffer_, source->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      resolved.texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                      VK_FILTER_NEAREST);
    } else {
      VkImageCopy copy{};
      copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.srcOffset = copy.dstOffset = {area.offset.x, area.offset.y, 0};
      copy.extent = {area.extent.width, area.extent.height, 1};
      cmd_copy_image_(command_buffer_, source->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      resolved.texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()),
                             barriers.data());
    resolved.initialized = true;
    ++resolves_;
    DumpImage(resolved.texture.image, VK_IMAGE_ASPECT_COLOR_BIT, width, height, false,
              fmt::format("{:08X}-rect-{}x{}+{}+{}", base_address << 12, area.extent.width,
                          area.extent.height, area.offset.x, area.offset.y),
              format, IsDumpAddress(base_address << 12));
  }

  // --wwe2k15_native_dump_address: one resolve destination, any frame.
  static bool IsDumpAddress(uint32_t address) {
    const std::string& text = REXCVAR_GET(wwe2k15_native_dump_address);
    return !text.empty() && std::strtoul(text.c_str(), nullptr, 16) == address;
  }

 private:
  // D3DRECT (x1, y1, x2, y2) clamped to width x height; false if empty.
  static bool ClampRect(const GuestRect& rect, uint32_t width, uint32_t height, VkRect2D& out) {
    const int32_t x1 = std::clamp(rect[0], 0, int32_t(width));
    const int32_t y1 = std::clamp(rect[1], 0, int32_t(height));
    const int32_t x2 = std::clamp(rect[2], 0, int32_t(width));
    const int32_t y2 = std::clamp(rect[3], 0, int32_t(height));
    if (x2 <= x1 || y2 <= y1) return false;
    out = {{x1, y1}, {uint32_t(x2 - x1), uint32_t(y2 - y1)}};
    return true;
  }

  // An RGBA8 image with an identity view (render targets and resolve copies
  // hold the shader's RGBA output directly), or R32_SFLOAT for depth resolves.
  Texture CreateTargetImage(uint32_t width, uint32_t height, VkImageUsageFlags usage,
                            VkFormat format = kBackBufferFormat,
                            VkComponentMapping components = {},
                            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
    Texture texture;
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = samples;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    if (!vk_ui::util::CreateDedicatedAllocationImage(device_, image_info,
                                                     vk_ui::util::MemoryPurpose::kDeviceLocal,
                                                     texture.image, texture.memory)) {
      return Texture{};
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = texture.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components = components;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    device_->functions().vkCreateImageView(device_->device(), &view_info, nullptr, &texture.view);
    return texture;
  }

  // A render target's depth buffer. Its first render pass (a clearing one)
  // takes it from UNDEFINED; after that it stays in
  // DEPTH_STENCIL_ATTACHMENT_OPTIMAL outside depth resolves.
  Texture CreateDepthImage(uint32_t width, uint32_t height,
                           VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
    Texture texture;
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kDepthFormat;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = samples;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (samples == VK_SAMPLE_COUNT_1_BIT) image_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!vk_ui::util::CreateDedicatedAllocationImage(device_, image_info,
                                                     vk_ui::util::MemoryPurpose::kDeviceLocal,
                                                     texture.image, texture.memory)) {
      REXGPU_ERROR("[native] could not create a {}x{} depth buffer", width, height);
      return Texture{};
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = texture.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kDepthFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1,
                                  0, 1};
    device_->functions().vkCreateImageView(device_->device(), &view_info, nullptr, &texture.view);
    return texture;
  }

  VkFramebuffer CreateFramebuffer(VkImageView view, VkImageView depth_view, uint32_t width,
                                  uint32_t height, VkFormat format = kBackBufferFormat) {
    const TargetPasses* passes = GetPasses(format);
    if (!passes) return VK_NULL_HANDLE;
    const std::array<VkImageView, 2> views{view, depth_view};
    VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb_info.renderPass = passes->clear;
    fb_info.attachmentCount = uint32_t(views.size());
    fb_info.pAttachments = views.data();
    fb_info.width = width;
    fb_info.height = height;
    fb_info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    device_->functions().vkCreateFramebuffer(device_->device(), &fb_info, nullptr, &framebuffer);
    return framebuffer;
  }

  // A target's framebuffer; multisampled targets add their resolve images
  // (attachments 2 and 3, CreateMsaaRenderPass).
  VkFramebuffer CreateFramebuffer(const RenderTarget& target) {
    if (target.samples == VK_SAMPLE_COUNT_1_BIT) {
      return CreateFramebuffer(target.image.view, target.depth.view, target.width,
                               target.height, target.format);
    }
    const TargetPasses* passes = GetPasses(target.format, target.samples);
    if (!passes) return VK_NULL_HANDLE;
    const std::array<VkImageView, 4> views{target.msaa_image.view, target.msaa_depth.view,
                                           target.image.view, target.depth.view};
    VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb_info.renderPass = passes->clear;
    fb_info.attachmentCount = uint32_t(views.size());
    fb_info.pAttachments = views.data();
    fb_info.width = target.width;
    fb_info.height = target.height;
    fb_info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    device_->functions().vkCreateFramebuffer(device_->device(), &fb_info, nullptr, &framebuffer);
    return framebuffer;
  }

  bool SubmitFrame() {
    const auto& dfn = device_->functions();
    if (ring_offset_) {
      vk_ui::util::FlushMappedMemoryRange(device_, ring_memory_, ring_memory_type_, 0,
                                          ring_memory_size_, ring_offset_);
    }
    if (stream_cache_dirty_end_ > stream_cache_dirty_begin_) {
      // From 0, like the ring: the helper needs atom-aligned ranges.
      vk_ui::util::FlushMappedMemoryRange(device_, stream_cache_memory_,
                                          stream_cache_memory_type_, 0,
                                          stream_cache_memory_size_, stream_cache_dirty_end_);
      stream_cache_dirty_begin_ = kStreamCacheSize;
      stream_cache_dirty_end_ = 0;
    }
    dfn.vkEndCommandBuffer(command_buffer_);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    auto queue = device_->AcquireQueue(device_->queue_family_graphics_compute(), 0);
    return dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence_) == VK_SUCCESS;
  }

  // The game's 256-entry gamma ramp (DC_LUT_30_COLOR words), which the
  // display controller applies at scan-out (section 20.7). The SDK's
  // CommandProcessor in rexgpu-native records the DC_LUT_* register writes and
  // the plugin exports them (wwe2k15_native_gamma_ramp). An identity ramp
  // until the command processor exists.
  void WriteGammaRamp(uint32_t* out) {
    using GammaRampFn = const uint32_t* (*)(rex::system::IGraphicsSystem*);
    static const auto get_gamma_ramp = [] {
      HMODULE plugin = GetModuleHandleW(L"rexgpu-native.dll");
      return plugin ? reinterpret_cast<GammaRampFn>(
                          GetProcAddress(plugin, "wwe2k15_native_gamma_ramp"))
                    : nullptr;
    }();
    auto* runtime = rex::Runtime::instance();
    const uint32_t* ramp = get_gamma_ramp && runtime
                               ? get_gamma_ramp(runtime->graphics_system())
                               : nullptr;
    if (ramp) {
      std::memcpy(out, ramp, kGammaRampSize);
      return;
    }
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t value = i * 0x3FF / 0xFF;
      out[i] = (value << 20) | (value << 10) | value;
    }
  }

  void SetFullViewport() {
    const auto& dfn = device_->functions();
    VkViewport viewport{0, 0, float(kBackBufferWidth), float(kBackBufferHeight), 0, 1};
    VkRect2D scissor{{0, 0}, {kBackBufferWidth, kBackBufferHeight}};
    dfn.vkCmdSetViewport(command_buffer_, 0, 1, &viewport);
    dfn.vkCmdSetScissor(command_buffer_, 0, 1, &scissor);
  }

  VkDeviceSize Allocate(VkDeviceSize size, VkDeviceSize alignment) {
    const VkDeviceSize offset = (ring_offset_ + alignment - 1) & ~(alignment - 1);
    if (offset + size > kUploadRingSize) {
      ++ring_overflows_;
      return VK_WHOLE_SIZE;
    }
    ring_offset_ = offset + size;
    return offset;
  }

  // --- Vertex streams (section 20.15) ----------------------------------------

  // Writes stream s as the pipeline reads it: big-endian 32-bit words to
  // little-endian, then the pipeline's repacked elements. out is
  // write-combined memory, so it is only written, never read back.
  static void ConvertStream(const Pipeline& pipeline, uint32_t s,
                            const NativeDrawState::Stream& stream, uint8_t* out) {
    const uint8_t* data = stream.bytes();
    const size_t size = stream.size();
    auto* dst = reinterpret_cast<uint32_t*>(out);
    // The fetch constant's endian mode (none, 8in16, 8in32, 16in32) decides
    // which 16-bit value of a word is the first component (the low bits).
    const uint32_t endian = stream.endian;
    auto load = [&](size_t i) {
      const uint32_t b0 = data[i], b1 = data[i + 1], b2 = data[i + 2], b3 = data[i + 3];
      switch (endian) {
        case 0: return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
        case 1: return b1 | (b0 << 8) | (b3 << 16) | (b2 << 24);
        case 3: return b2 | (b3 << 8) | (b0 << 16) | (b1 << 24);
        default: return b3 | (b2 << 8) | (b1 << 16) | (b0 << 24);
      }
    };
    if (endian == 2) {
      for (size_t i = 0; i + 4 <= size; i += 4) {
        uint32_t word;
        std::memcpy(&word, data + i, 4);
        dst[i / 4] = __builtin_bswap32(word);
      }
    } else {
      for (size_t i = 0; i + 4 <= size; i += 4) dst[i / 4] = load(i);
    }
    const uint32_t stride = stream.stride;
    for (const auto& element : pipeline.repacked_normals) {
      if (element.stream != s || element.offset % 4 || stride % 4) continue;
      for (size_t at = element.offset; at + 4 <= size; at += stride) {
        dst[at / 4] = RepackNormal(load(at));
      }
    }
    for (const auto& element : pipeline.swapped_halves) {
      if (element.stream != s || element.offset % 4 || stride % 4) continue;
      for (size_t at = element.offset; at + 4 * element.words <= size; at += stride) {
        for (uint32_t w = 0; w < element.words; ++w) {
          const uint32_t v = load(at + 4 * w);
          dst[at / 4 + w] = (v << 16) | (v >> 16);
        }
      }
    }
  }

  // A guest vertex buffer range, converted once into stream_cache_buffer_ and
  // reused while its bytes (XXH3) stay the same.
  struct CachedStream {
    VkDeviceSize offset = 0, capacity = 0;
    uint64_t hash = 0;
    uint64_t drawn_frame = 0;    // the last frame a draw read it
    uint64_t changed_frame = 0;  // the last frame its contents changed
    uint32_t changes = 0;        // changes in consecutive frames
  };
  // The demo uses about 6 MB (section 20.15).
  static constexpr VkDeviceSize kStreamCacheSize = 32 * 1024 * 1024;
  // Ranges rewritten this many frames in a row go through the ring for good
  // (until the next flush): hashing them only costs time.
  static constexpr uint32_t kDynamicChanges = 3;

  // Points buffer/offset at stream s's converted data in the cache, converting
  // it first if it is new or changed. False: use the ring instead (the range
  // is dynamic, too big, changed after a draw this frame already read it, or
  // the cache is full).
  bool UseCachedStream(const Pipeline& pipeline, uint32_t s, const NativeDrawState::Stream& stream,
                       VkBuffer& buffer, VkDeviceSize& offset) {
    const size_t size = stream.size();
    if (!stream_cache_mapping_ || size > kStreamCacheSize / 16) return false;
    // The converted bytes depend on the range, its layout and the
    // pipeline's repacked elements for this stream.
    const uint32_t layout[4] = {stream.address, uint32_t(size), stream.stride, stream.endian};
    uint64_t key = Fnv1a(layout, sizeof(layout));
    for (const auto* list : {&pipeline.repacked_normals, &pipeline.swapped_halves}) {
      for (const auto& element : *list) {
        if (element.stream == s) key = Fnv1a(&element, sizeof(element), key);
      }
      key = Fnv1a("|", 1, key);
    }
    CachedStream& entry = stream_cache_[key];
    if (entry.changes >= kDynamicChanges) return false;
    const uint64_t hash = XXH3_64bits(stream.bytes(), size);
    const uint64_t frame = recording_frame_;
    if (entry.capacity && entry.hash == hash) {
      entry.drawn_frame = frame;
      buffer = stream_cache_buffer_;
      offset = entry.offset;
      cached_bytes_ += size;
      return true;
    }
    if (entry.capacity) {
      // Changed: count consecutive frames with changes.
      entry.changes = entry.changed_frame + 1 >= frame ? entry.changes + 1 : 1;
      entry.changed_frame = frame;
      // An earlier draw of this frame reads the old contents (the GPU has
      // not run it yet), so they cannot be overwritten now.
      if (entry.drawn_frame == frame || entry.changes >= kDynamicChanges) return false;
    }
    if (entry.capacity < size) {
      const VkDeviceSize at = (stream_cache_offset_ + 15) & ~VkDeviceSize(15);
      if (at + size > kStreamCacheSize) {
        // Full: start again at the next frame, when no draw reads it.
        stream_cache_flush_ = true;
        return false;
      }
      stream_cache_offset_ = at + size;
      entry.offset = at;
      entry.capacity = size;
    }
    ConvertStream(pipeline, s, stream, stream_cache_mapping_ + entry.offset);
    stream_cache_dirty_begin_ = std::min(stream_cache_dirty_begin_, entry.offset);
    stream_cache_dirty_end_ = std::max(stream_cache_dirty_end_, entry.offset + size);
    entry.hash = hash;
    entry.drawn_frame = frame;
    buffer = stream_cache_buffer_;
    offset = entry.offset;
    vertex_bytes_ += size;
    return true;
  }

  // --- Shaders and pipelines -------------------------------------------------

  VkShaderModule GetShader(uint64_t hash) {
    auto it = shaders_.find(hash);
    if (it != shaders_.end()) return it->second;
    VkShaderModule module = VK_NULL_HANDLE;
    const auto path = std::filesystem::path(shader_dir_) / fmt::format("{:016X}.spv", hash);
    std::ifstream file(path, std::ios::binary);
    if (file) {
      std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
      std::vector<uint32_t> words((bytes.size() + 3) / 4);
      std::memcpy(words.data(), bytes.data(), bytes.size());
      module = vk_ui::util::CreateShaderModule(device_, words.data(), bytes.size());
      shader_inputs_[hash] = ReflectInputs(words.data(), words.size());
    }
    if (module == VK_NULL_HANDLE && missing_shaders_.emplace(hash, true).second) {
      REXGPU_WARN("[native] no SPIR-V for shader {:016X} ({})", hash, path.string());
    }
    shaders_.emplace(hash, module);
    return module;
  }

  const Pipeline& GetPipeline(const NativeDrawState& state, VkPrimitiveTopology topology,
                              VkFormat format, VkSampleCountFlagBits samples) {
    uint64_t key = Fnv1a(&state.vertex_shader, 8);
    key = Fnv1a(&format, sizeof(format), key);
    key = Fnv1a(&samples, sizeof(samples), key);
    key = Fnv1a(&state.pixel_shader, 8, key);
    key = Fnv1a(&state.declaration_key, 8, key);
    for (const auto& stream : state.streams) key = Fnv1a(&stream.stride, 4, key);
    key = Fnv1a(&topology, sizeof(topology), key);
    const bool stencil = state.depth_control & 1;
    const uint32_t render_state[8] = {state.blend_control,
                                      state.mode_control & (kCullBits | kRestartBit),
                                      state.color_mask & 0xF,
                                      uint32_t(GetAlphaTest(state)),
                                      state.color_control & kAlphaToMaskBit,
                                      state.depth_control,
                                      stencil ? state.stencil_ref_mask & 0xFFFFFF : 0,
                                      stencil ? state.stencil_ref_mask_bf & 0xFFFFFF : 0};
    key = Fnv1a(render_state, sizeof(render_state), key);
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) return it->second;
    Pipeline pipeline;
    pipeline.pipeline = CreatePipeline(state, topology, format, samples, pipeline);
    return pipelines_.emplace(key, std::move(pipeline)).first->second;
  }

  // Fills out's element lists (the words repacked on upload).
  VkPipeline CreatePipeline(const NativeDrawState& state, VkPrimitiveTopology topology,
                            VkFormat format, VkSampleCountFlagBits samples, Pipeline& out) {
    auto& repacked_normals = out.repacked_normals;
    const TargetPasses* passes = GetPasses(format, samples);
    if (!passes) return VK_NULL_HANDLE;
    VkShaderModule vs = GetShader(state.vertex_shader);
    VkShaderModule ps = GetShader(state.pixel_shader);
    if (vs == VK_NULL_HANDLE || ps == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    const ShaderInputs& inputs = shader_inputs_[state.vertex_shader];

    // Each declared element the vertex shader reads, at the shader's location
    // for its semantic (section 20.9). Elements it does not read are left out.
    std::vector<VkVertexInputAttributeDescription> attributes;
    bool packed_normals = false;
    for (size_t i = 0; i + 12 <= state.declaration.size(); i += 12) {
      const uint8_t* e = state.declaration.data() + i;
      const uint32_t stream = (e[0] << 8) | e[1];
      if (stream == 0xFF) break;
      if (stream >= state.streams.size() || !state.streams[stream].stride) continue;
      const uint32_t offset = (e[2] << 8) | e[3];
      const uint32_t type = (uint32_t(e[4]) << 24) | (e[5] << 16) | (e[6] << 8) | e[7];
      if (e[9] >= std::size(kUsageSemantics)) continue;
      auto input = inputs.find(fmt::format("{}{}", kUsageSemantics[e[9]], e[10]));
      if (input == inputs.end()) continue;
      const VkFormat format = VertexFormat(type, input->second.integer);
      if (format == VK_FORMAT_UNDEFINED) {
        REXGPU_WARN("[native] unsupported vertex element usage {}.{} type {:08X} ({})", e[9],
                    e[10], type, input->second.integer ? "uint" : "float");
        return VK_NULL_HANDLE;
      }
      // Packed normals, tangents and binormals: the shader unpacks them when
      // SPEC_CONSTANT_R11G11B10_NORMAL is set.
      const uint32_t xenos_format = type & 0x3F;
      if (input->second.integer && (xenos_format == 16 || xenos_format == 17)) {
        packed_normals = true;
        if (xenos_format == 17) repacked_normals.push_back({stream, offset});
      }
      if (xenos_format == 25 || xenos_format == 31) out.swapped_halves.push_back({stream, offset, 1});
      if (xenos_format == 26 || xenos_format == 32) out.swapped_halves.push_back({stream, offset, 2});
      attributes.push_back({input->second.location, stream, format, offset});
      REXGPU_INFO("[native]   element {}{}: stream {} stride {} endian {} offset {} type {:08X} "
                  "-> {}",
                  kUsageSemantics[e[9]], e[10], stream, state.streams[stream].stride,
                  state.streams[stream].endian, offset, type, uint32_t(format));
    }
    // One binding per D3D stream that has data.
    std::vector<VkVertexInputBindingDescription> bindings;
    for (uint32_t s = 0; s < state.streams.size(); ++s) {
      if (state.streams[s].stride) {
        bindings.push_back({s, state.streams[s].stride, VK_VERTEX_INPUT_RATE_VERTEX});
      }
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    // Specialization constant 0 (SPEC_CONSTANT_* flags).
    const uint32_t spec_value = (GetAlphaTest(state) == AlphaTest::kOn ? kSpecAlphaTest : 0) |
                                (packed_normals ? kSpecPackedNormals : 0);
    VkSpecializationMapEntry spec_entry{0, 0, sizeof(uint32_t)};
    VkSpecializationInfo spec{1, &spec_entry, sizeof(spec_value), &spec_value};
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vs, "main", &spec};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main", &spec};

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = topology;
    // Strips and fans restart at index 0xFFFFFFFF, which NativeOnIndexedDraw
    // writes for the game's reset index (section 20.21). Lists cannot restart
    // in core Vulkan.
    input_assembly.primitiveRestartEnable =
        (state.mode_control & kRestartBit) && topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // PA_SU_SC_MODE_CNTL: bit 0 culls front faces, bit 1 back faces, bit 2
    // makes clockwise triangles the front ones.
    raster.cullMode = ((state.mode_control & 1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                      ((state.mode_control & 2) ? VK_CULL_MODE_BACK_BIT : 0);
    raster.frontFace = (state.mode_control & 4) ? VK_FRONT_FACE_CLOCKWISE
                                                : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;
    // RB_COLORCONTROL alpha-to-mask: coverage from alpha (section 20.14).
    // The Xenos dither offsets (bits 24-31) are not reproduced.
    multisample.alphaToCoverageEnable = (state.color_control & kAlphaToMaskBit) != 0;
    // Blending and color writes from the device's register shadow (section 20.4).
    VkPipelineColorBlendAttachmentState blend_attachment = BlendAttachment(state);
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkPipelineDepthStencilStateCreateInfo depth_stencil = DepthStencilState(state);
    std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = uint32_t(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDepthStencilState = &depth_stencil;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    info.renderPass = passes->clear;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (device_->functions().vkCreateGraphicsPipelines(device_->device(), VK_NULL_HANDLE, 1,
                                                       &info, nullptr, &pipeline) != VK_SUCCESS) {
      REXGPU_WARN("[native] pipeline creation failed for vs {:016X} ps {:016X}",
                  state.vertex_shader, state.pixel_shader);
      return VK_NULL_HANDLE;
    }
    REXGPU_INFO("[native] pipeline vs {:016X} ps {:016X}: blend {:08X} color control {:08X} "
                "mask {:X} alpha ref {:08X} depth control {:08X} cull {:X}",
                state.vertex_shader, state.pixel_shader, state.blend_control,
                state.color_control, state.color_mask & 0xF, state.alpha_ref,
                state.depth_control, state.mode_control & kCullBits);
    return pipeline;
  }

  // --- Textures --------------------------------------------------------------

  VkDescriptorSet WriteTextureSet(const NativeDrawState& state) {
    const auto& dfn = device_->functions();
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = frame_descriptor_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &texture_set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (dfn.vkAllocateDescriptorSets(device_->device(), &alloc, &set) != VK_SUCCESS) {
      ++descriptor_overflows_;
      return VK_NULL_HANDLE;
    }
    std::array<VkDescriptorImageInfo, kFetchSlots> images_2d, images_3d, images_cube, samplers;
    for (uint32_t slot = 0; slot < kFetchSlots; ++slot) {
      VkImageView view = GetTexture(state.texture_fetch[slot]);
      if (Dumping() && view) {
        xenos::xe_gpu_texture_fetch_t fetch;
        std::memcpy(&fetch, state.texture_fetch[slot].data(), sizeof(fetch));
        REXGPU_INFO("[native] draw {}: slot {} base {:08X} fmt {} {}x{} tiled {} pitch {} "
                    "mips {}-{} packed {} swizzle {:03X} mip base {:08X} clamp {}{}{} "
                    "border {} filter {}{}{} sign {}{}{}{}",
                    drawn_, slot, fetch.base_address << 12, uint32_t(fetch.format),
                    fetch.size_2d.width + 1, fetch.size_2d.height + 1, uint32_t(fetch.tiled),
                    uint32_t(fetch.pitch), uint32_t(fetch.mip_min_level),
                    uint32_t(fetch.mip_max_level), uint32_t(fetch.packed_mips),
                    uint32_t(fetch.swizzle), fetch.mip_address << 12, uint32_t(fetch.clamp_x),
                    uint32_t(fetch.clamp_y), uint32_t(fetch.clamp_z),
                    uint32_t(fetch.border_color), uint32_t(fetch.mag_filter),
                    uint32_t(fetch.min_filter), uint32_t(fetch.mip_filter),
                    uint32_t(fetch.sign_x), uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
                    uint32_t(fetch.sign_w));
        // Linear DXT1 contents as guest memory holds them (endian swap not
        // applied), for offline decoding.
        if (fetch.format == xenos::TextureFormat::k_DXT1 && !fetch.tiled) {
          const uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
          const size_t size = size_t((width + 3) / 4) * ((height + 3) / 4) * 8;
          const auto path = std::filesystem::absolute(fmt::format(
              "native-dumps/frame-{}/tex-{:08X}-dxt1-{}x{}-endian{}.bin", recording_frame_,
              fetch.base_address << 12, width, height, uint32_t(fetch.endianness)));
          std::filesystem::create_directories(path.parent_path());
          if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
            std::fwrite(rex::system::kernel_memory()->TranslatePhysical(fetch.base_address << 12),
                        1, size, file);
            std::fclose(file);
          }
        }
      }
      images_2d[slot] = {VK_NULL_HANDLE, view ? view : dummy_2d_.view,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      images_3d[slot] = {VK_NULL_HANDLE, dummy_3d_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      images_cube[slot] = {VK_NULL_HANDLE, dummy_cube_.view,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      samplers[slot] = {GetSampler(state.texture_fetch[slot]), VK_NULL_HANDLE,
                        VK_IMAGE_LAYOUT_UNDEFINED};
    }
    std::array<VkWriteDescriptorSet, 4> writes{};
    const std::array<const VkDescriptorImageInfo*, 4> infos{images_2d.data(), images_3d.data(),
                                                            images_cube.data(), samplers.data()};
    for (uint32_t i = 0; i < 4; ++i) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, kFetchSlots,
                   i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                   infos[i], nullptr, nullptr};
    }
    dfn.vkUpdateDescriptorSets(device_->device(), uint32_t(writes.size()), writes.data(), 0,
                               nullptr);
    return set;
  }

  // A sampler with the fetch constant's addressing and filtering (section
  // 20.8). Repeat for every texture blended a tile's edge with its opposite
  // edge: the loading screen's picture is two tiles that use mirrored repeat.
  // Not handled: anisotropy, LOD bias, the halfway clamps (as edge clamps),
  // mirror-once clamps (as mirrored repeat) and the shader's filter overrides.
  VkSampler GetSampler(const std::array<uint32_t, 6>& words) {
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words.data(), sizeof(fetch));
    if (fetch.type != xenos::FetchConstantType::kTexture) return linear_wrap_sampler_;
    auto address_mode = [](xenos::ClampMode mode) {
      switch (mode) {
        case xenos::ClampMode::kRepeat:
          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case xenos::ClampMode::kMirroredRepeat:
        case xenos::ClampMode::kMirrorClampToEdge:
        case xenos::ClampMode::kMirrorClampToHalfway:
          return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case xenos::ClampMode::kClampToBorder:
        case xenos::ClampMode::kMirrorClampToBorder:
          return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:
          return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      }
    };
    auto filter = [](xenos::TextureFilter value) {
      return value == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    };
    const uint32_t key = uint32_t(fetch.clamp_x) | (uint32_t(fetch.clamp_y) << 3) |
                         (uint32_t(fetch.clamp_z) << 6) | (uint32_t(fetch.mag_filter) << 9) |
                         (uint32_t(fetch.min_filter) << 11) | (uint32_t(fetch.mip_filter) << 13) |
                         (uint32_t(fetch.border_color) << 15);
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = filter(fetch.mag_filter);
    info.minFilter = filter(fetch.min_filter);
    info.mipmapMode = fetch.mip_filter == xenos::TextureFilter::kPoint
                          ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                          : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = address_mode(fetch.clamp_x);
    info.addressModeV = address_mode(fetch.clamp_y);
    info.addressModeW = address_mode(fetch.clamp_z);
    info.maxLod = fetch.mip_filter == xenos::TextureFilter::kBaseMap ? 0.0f : VK_LOD_CLAMP_NONE;
    info.borderColor = fetch.border_color == xenos::BorderColor::k_ABGR_White
                           ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                           : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler sampler = VK_NULL_HANDLE;
    if (device_->functions().vkCreateSampler(device_->device(), &info, nullptr, &sampler) !=
        VK_SUCCESS) {
      sampler = linear_wrap_sampler_;
    }
    samplers_.emplace(key, sampler);
    return sampler;
  }

  // Hashes 64 bytes out of every 4 KB of the texture's base level. The size
  // follows the upload's layout so that the read stays inside the texture
  // (unallocated guest pages are not readable). Unsupported formats: no hash.
  static uint64_t SampleContents(const xenos::xe_gpu_texture_fetch_t& fetch, uint64_t hash) {
    uint32_t block = 1, block_bytes = 4;
    switch (fetch.format) {
      case xenos::TextureFormat::k_DXT1: block = 4; block_bytes = 8; break;
      case xenos::TextureFormat::k_DXT2_3:
      case xenos::TextureFormat::k_DXT4_5: block = 4; block_bytes = 16; break;
      case xenos::TextureFormat::k_8_8_8_8:
      case xenos::TextureFormat::k_16_16:
      case xenos::TextureFormat::k_32_FLOAT: break;
      case xenos::TextureFormat::k_8: block_bytes = 1; break;
      case xenos::TextureFormat::k_16_16_16_16: block_bytes = 8; break;
      default: return hash;
    }
    const uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
    const uint32_t pitch_texels = std::max<uint32_t>(fetch.pitch * 32, width);
    uint64_t pitch_blocks = (pitch_texels + block - 1) / block;
    uint64_t blocks_y = (height + block - 1) / block;
    uint64_t row_bytes = pitch_blocks * block_bytes;
    if (fetch.tiled) {
      // Whole 32x32-block tiles.
      pitch_blocks = (pitch_blocks + 31) & ~uint64_t(31);
      blocks_y = (blocks_y + 31) & ~uint64_t(31);
      row_bytes = pitch_blocks * block_bytes;
    } else {
      row_bytes = (row_bytes + 255) & ~uint64_t(255);
    }
    const uint64_t size = std::min<uint64_t>(row_bytes * blocks_y, 16u << 20);
    const uint8_t* data =
        rex::system::kernel_memory()->TranslatePhysical(fetch.base_address << 12);
    for (uint64_t offset = 0; offset + 64 <= size; offset += 4096) {
      hash = Fnv1a(data + offset, 64, hash);
    }
    return hash;
  }

  // Returns a view for a 2D texture fetch constant, or VK_NULL_HANDLE.
  VkImageView GetTexture(const std::array<uint32_t, 6>& words) {
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words.data(), sizeof(fetch));
    if (fetch.type != xenos::FetchConstantType::kTexture &&
        fetch.type != xenos::FetchConstantType::kInvalidTexture) {
      return VK_NULL_HANDLE;
    }
    if (fetch.dimension != xenos::DataDimension::k2DOrStacked || !fetch.base_address) {
      return VK_NULL_HANDLE;
    }
    // Resolved render targets replace what guest memory holds there.
    if (auto resolved = resolved_.find(PhysicalPage(fetch.base_address));
        resolved != resolved_.end() && resolved->second.initialized) {
      if (Dumping() && resolved->second.format != kBackBufferFormat) {
        REXGPU_INFO("[native] draw {}: samples resolved {:08X} (format {}) as fmt {} sign "
                    "{}{}{}{} int {} exp {} swizzle {:03X} endian {}",
                    drawn_, fetch.base_address << 12, uint32_t(resolved->second.format),
                    uint32_t(fetch.format), uint32_t(fetch.sign_x), uint32_t(fetch.sign_y),
                    uint32_t(fetch.sign_z), uint32_t(fetch.sign_w), uint32_t(fetch.num_format),
                    int32_t(fetch.exp_adjust), uint32_t(fetch.swizzle),
                    uint32_t(fetch.endianness));
      }
      return resolved->second.texture.view;
    }
    // Address, format, size and swizzle (the swizzle selects the view
    // components). A sample of the contents decides whether the cached copy is
    // current: the game reuses memory for new textures of the same size and
    // format, and some textures change every frame.
    const std::array<uint32_t, 4> key_words{words[0], words[1], words[2], fetch.swizzle};
    const uint64_t key = Fnv1a(key_words.data(), sizeof(key_words));
    // Contents are sampled once per frame: a demo frame binds the same few
    // hundred textures in about 1,300 draws (section 20.9).
    if (auto seen = frame_textures_.find(key); seen != frame_textures_.end()) return seen->second;
    VkImageView view = LookUpTexture(fetch, key);
    frame_textures_.emplace(key, view);
    return view;
  }

  VkImageView LookUpTexture(const xenos::xe_gpu_texture_fetch_t& fetch, uint64_t key) {
    const uint64_t contents =
        fetch.type == xenos::FetchConstantType::kTexture ? SampleContents(fetch, 0) : 0;
    auto it = textures_.find(key);
    if (it != textures_.end()) {
      if (it->second.contents == contents) return it->second.texture.view;
      Retire(it->second.texture);
      textures_.erase(it);
    }
    const auto create_start = std::chrono::steady_clock::now();
    const Texture texture = CreateTexture(fetch);
    texture_create_time_ += std::chrono::steady_clock::now() - create_start;
    ++textures_created_;
    textures_.emplace(key, CachedTexture{texture, contents});
    return texture.view;
  }

  struct CachedTexture {
    Texture texture;
    uint64_t contents = 0;
  };

  // Images still used by this frame's commands; freed after its fence.
  void Retire(const Texture& texture) {
    if (texture.image != VK_NULL_HANDLE) retired_.push_back(texture);
  }

  void FreeRetired() {
    const auto& dfn = device_->functions();
    const VkDevice device = device_->device();
    for (const Texture& texture : retired_) {
      if (texture.view) dfn.vkDestroyImageView(device, texture.view, nullptr);
      if (texture.image) dfn.vkDestroyImage(device, texture.image, nullptr);
      if (texture.memory) dfn.vkFreeMemory(device, texture.memory, nullptr);
    }
    retired_.clear();
  }

  Texture CreateTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
    Texture texture;
    VkFormat format;
    uint32_t block = 1, block_bytes = 4;
    switch (fetch.format) {
      case xenos::TextureFormat::k_DXT1: format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; block = 4; block_bytes = 8; break;
      case xenos::TextureFormat::k_DXT2_3: format = VK_FORMAT_BC2_UNORM_BLOCK; block = 4; block_bytes = 16; break;
      case xenos::TextureFormat::k_DXT4_5: format = VK_FORMAT_BC3_UNORM_BLOCK; block = 4; block_bytes = 16; break;
      case xenos::TextureFormat::k_8_8_8_8: format = VK_FORMAT_R8G8B8A8_UNORM; break;
      case xenos::TextureFormat::k_8: format = VK_FORMAT_R8_UNORM; block_bytes = 1; break;
      case xenos::TextureFormat::k_32_FLOAT: format = VK_FORMAT_R32_SFLOAT; break;
      // 16-bit components: signed X picks SNORM for all (section 20.16).
      case xenos::TextureFormat::k_16_16:
        format = fetch.sign_x == xenos::TextureSign::kSigned ? VK_FORMAT_R16G16_SNORM
                                                             : VK_FORMAT_R16G16_UNORM;
        break;
      case xenos::TextureFormat::k_16_16_16_16:
        format = fetch.sign_x == xenos::TextureSign::kSigned ? VK_FORMAT_R16G16B16A16_SNORM
                                                             : VK_FORMAT_R16G16B16A16_UNORM;
        block_bytes = 8;
        break;
      default:
        if (unsupported_formats_.insert(uint32_t(fetch.format)).second) {
          REXGPU_WARN("[native] unsupported texture format {} ({}x{} at {:08X})",
                      uint32_t(fetch.format), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                      fetch.base_address << 12);
        }
        return texture;
    }
    const uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
    if (fetch.format == xenos::TextureFormat::k_16_16 ||
        fetch.format == xenos::TextureFormat::k_16_16_16_16) {
      REXGPU_INFO("[native] 16-bit texture {:08X}: format {} {}x{} sign {}{}{}{} int {} exp {} "
                  "swizzle {:03X} endian {} tiled {}",
                  fetch.base_address << 12, uint32_t(fetch.format), width, height,
                  uint32_t(fetch.sign_x), uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
                  uint32_t(fetch.sign_w), uint32_t(fetch.num_format), int32_t(fetch.exp_adjust),
                  uint32_t(fetch.swizzle), uint32_t(fetch.endianness), uint32_t(fetch.tiled));
    }
    // Mip levels 1 and up are stored under mip_address (section 20.20).
    const uint32_t levels =
        1 + (fetch.mip_address && REXCVAR_GET(wwe2k15_native_mips)
                 ? std::min<uint32_t>(fetch.mip_max_level, Log2Floor(std::max(width, height)))
                 : 0);
    // Levels whose shorter side is 16 texels or less share one 32x32 tile, the
    // packed mip tail, stored like the level packed_level.
    const uint32_t packed_level = fetch.packed_mips ? PackedMipLevel(width, height) : UINT32_MAX;
    const uint32_t pitch_texels = std::max<uint32_t>(fetch.pitch * 32, width);

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = levels;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!vk_ui::util::CreateDedicatedAllocationImage(device_, image_info,
                                                     vk_ui::util::MemoryPurpose::kDeviceLocal,
                                                     texture.image, texture.memory)) {
      return Texture{};
    }
    // Guest data (physical memory) -> staging with endian swap, level by level.
    auto* memory = rex::system::kernel_memory();
    if (fetch.tiled) {
      REXGPU_INFO("[native] untiling texture {:08X}: format {} {}x{} pitch {} levels {}{}",
                  fetch.base_address << 12, uint32_t(fetch.format), width, height,
                  pitch_texels, levels, packed_level != UINT32_MAX ? " packed" : "");
    }
    const uint32_t bpb_log2 = block_bytes == 1 ? 0 : block_bytes == 4 ? 2 : block_bytes == 8 ? 3 : 4;
    std::vector<uint8_t> data;
    std::vector<VkBufferImageCopy> regions;
    // Copies one level, whose storage starts at address with this pitch (in
    // blocks), from block (offset_x, offset_y) of that storage.
    auto copy_level = [&](uint32_t level, uint32_t address, uint32_t pitch_blocks,
                          uint32_t offset_x, uint32_t offset_y) {
      const uint32_t level_width = std::max(width >> level, 1u);
      const uint32_t level_height = std::max(height >> level, 1u);
      const uint32_t blocks_x = (level_width + block - 1) / block;
      const uint32_t blocks_y = (level_height + block - 1) / block;
      const uint32_t tight_row = blocks_x * block_bytes;
      // Linear rows are padded to 256 bytes.
      const uint32_t row_bytes = (pitch_blocks * block_bytes + 255) & ~255u;
      const size_t start = data.size();
      data.resize(start + size_t(tight_row) * blocks_y);
      const uint8_t* src = memory->TranslatePhysical(address);
      for (uint32_t y = 0; y < blocks_y; ++y) {
        uint8_t* dst = data.data() + start + size_t(y) * tight_row;
        if (fetch.tiled) {
          // Tiled: each block is found through the 32x32-block tile layout.
          for (uint32_t x = 0; x < blocks_x; ++x) {
            const int32_t offset = TiledOffset2D(int32_t(offset_x + x), int32_t(offset_y + y),
                                                 pitch_blocks, bpb_log2);
            std::memcpy(dst + size_t(x) * block_bytes, src + offset, block_bytes);
          }
        } else {
          std::memcpy(dst, src + size_t(offset_y + y) * row_bytes + size_t(offset_x) * block_bytes,
                      tight_row);
        }
      }
      VkBufferImageCopy region{};
      region.bufferOffset = start;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
      region.imageExtent = {level_width, level_height, 1};
      regions.push_back(region);
    };
    // The base level uses the fetch constant's pitch. A base inside the mip
    // tail sits at its packed offset there.
    uint32_t tail_x = 0, tail_y = 0;
    if (packed_level == 0) PackedMipOffset(width, height, 0, tail_x, tail_y);
    copy_level(0, fetch.base_address << 12, (pitch_texels + block - 1) / block, tail_x / block,
               tail_y / block);
    // Mips are stored as if the base size were a power of two: level n has
    // next_pow2(size) >> n texels, padded to 32x32 blocks and to 4 KB.
    uint32_t mip_offset = 0;
    for (uint32_t level = 1; level < levels; ++level) {
      const uint32_t stored = std::min(level, packed_level);
      auto stored_blocks = [&](uint32_t size, uint32_t stored_level) {
        const uint32_t texels = std::max(NextPow2(size) >> stored_level, 1u);
        return (((texels + block - 1) / block) + 31) & ~31u;
      };
      const uint32_t pitch_blocks = stored_blocks(width, stored);
      tail_x = tail_y = 0;
      if (level >= packed_level) PackedMipOffset(width, height, level, tail_x, tail_y);
      copy_level(level, (fetch.mip_address << 12) + mip_offset, pitch_blocks, tail_x / block,
                 tail_y / block);
      if (level < packed_level) {
        uint32_t row_bytes = pitch_blocks * block_bytes;
        if (!fetch.tiled) row_bytes = (row_bytes + 255) & ~255u;
        mip_offset += (row_bytes * stored_blocks(height, stored) + 4095) & ~4095u;
      }
    }
    SwapEndian(data, fetch.endianness);
    if (!UploadImage(texture.image, levels, regions, data)) {
      return Texture{};
    }
    if (levels > 1) ++mipped_textures_;
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = texture.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    const bool single_channel = fetch.format == xenos::TextureFormat::k_8 ||
                                fetch.format == xenos::TextureFormat::k_32_FLOAT;
    view_info.components = {ViewSwizzle(fetch.swizzle, 0, single_channel),
                            ViewSwizzle(fetch.swizzle, 1, single_channel),
                            ViewSwizzle(fetch.swizzle, 2, single_channel),
                            ViewSwizzle(fetch.swizzle, 3, single_channel)};
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    device_->functions().vkCreateImageView(device_->device(), &view_info, nullptr, &texture.view);
    return texture;
  }

  // Guest fetch swizzle (3 bits per component, XE_GPU_TEXTURE_SWIZZLE) to a view
  // component, as the SDK's GuestToHostSwizzle does. The host formats keep the
  // guest components in RGBA order, except the one-component k_8 and
  // k_32_FLOAT, whose R replicates.
  static VkComponentSwizzle ViewSwizzle(uint32_t swizzle, uint32_t component,
                                        bool single_channel) {
    const uint32_t guest = (swizzle >> (3 * component)) & 0b111;
    if (guest >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
      // 6 and 7 are invalid; treat them as 0 and 1 like the SDK.
      return (guest & 1) ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO;
    }
    if (single_channel) return VK_COMPONENT_SWIZZLE_R;
    static constexpr VkComponentSwizzle kComponents[] = {
        VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_A};
    return kComponents[guest];
  }

  static void SwapEndian(std::vector<uint8_t>& data, xenos::Endian endian) {
    switch (endian) {
      case xenos::Endian::k8in16:
        for (size_t i = 0; i + 1 < data.size(); i += 2) std::swap(data[i], data[i + 1]);
        break;
      case xenos::Endian::k8in32:
        for (size_t i = 0; i + 3 < data.size(); i += 4) {
          std::swap(data[i], data[i + 3]);
          std::swap(data[i + 1], data[i + 2]);
        }
        break;
      case xenos::Endian::k16in32:
        for (size_t i = 0; i + 3 < data.size(); i += 4) {
          std::swap(data[i], data[i + 2]);
          std::swap(data[i + 1], data[i + 3]);
        }
        break;
      default:
        break;
    }
  }

  bool UploadImage(VkImage image, uint32_t width, uint32_t height,
                   const std::vector<uint8_t>& data) {
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    return UploadImage(image, 1, {region}, data);
  }

  // Synchronous upload through a staging buffer on a separate command buffer;
  // regions fill levels 0 to levels - 1.
  bool UploadImage(VkImage image, uint32_t levels, const std::vector<VkBufferImageCopy>& regions,
                   const std::vector<uint8_t>& data) {
    const auto& dfn = device_->functions();
    const VkDevice device = device_->device();
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkDeviceSize staging_size = 0;
    uint32_t staging_type = 0;
    if (!vk_ui::util::CreateDedicatedAllocationBuffer(
            device_, data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            vk_ui::util::MemoryPurpose::kUpload, staging, staging_memory, &staging_type,
            &staging_size)) {
      return false;
    }
    void* mapping = nullptr;
    dfn.vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapping);
    std::memcpy(mapping, data.data(), data.size());
    vk_ui::util::FlushMappedMemoryRange(device_, staging_memory, staging_type, 0, staging_size);
    dfn.vkUnmapMemory(device, staging_memory);

    dfn.vkResetCommandPool(device, upload_pool_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn.vkBeginCommandBuffer(upload_buffer_, &begin);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    dfn.vkCmdPipelineBarrier(upload_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    dfn.vkCmdCopyBufferToImage(upload_buffer_, staging, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()),
                               regions.data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn.vkCmdPipelineBarrier(upload_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    dfn.vkEndCommandBuffer(upload_buffer_);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &upload_buffer_;
    dfn.vkResetFences(device, 1, &upload_fence_);
    bool ok;
    {
      auto queue = device_->AcquireQueue(device_->queue_family_graphics_compute(), 0);
      ok = dfn.vkQueueSubmit(queue.queue(), 1, &submit, upload_fence_) == VK_SUCCESS;
    }
    if (ok) dfn.vkWaitForFences(device, 1, &upload_fence_, VK_TRUE, UINT64_MAX);
    dfn.vkDestroyBuffer(device, staging, nullptr);
    dfn.vkFreeMemory(device, staging_memory, nullptr);
    return ok;
  }

  bool CreateDummy(Texture& texture, VkImageType type, VkImageViewType view_type,
                   uint32_t layers, VkImageCreateFlags flags) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.flags = flags;
    image_info.imageType = type;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {1, 1, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = layers;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!vk_ui::util::CreateDedicatedAllocationImage(device_, image_info,
                                                     vk_ui::util::MemoryPurpose::kDeviceLocal,
                                                     texture.image, texture.memory)) {
      return false;
    }
    std::vector<uint8_t> white(4, 0xFF);  // opaque white
    // UploadImage writes layer 0 only; the other cube faces stay undefined but
    // are never sampled by the 2D-only paths used so far.
    if (!UploadImage(texture.image, 1, 1, white)) return false;
    if (layers > 1) {
      // Transition the remaining layers too, so the view is valid.
      TransitionLayers(texture.image, layers);
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = texture.image;
    view_info.viewType = view_type;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    return device_->functions().vkCreateImageView(device_->device(), &view_info, nullptr,
                                                  &texture.view) == VK_SUCCESS;
  }

  void TransitionLayers(VkImage image, uint32_t layers) {
    const auto& dfn = device_->functions();
    const VkDevice device = device_->device();
    dfn.vkResetCommandPool(device, upload_pool_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn.vkBeginCommandBuffer(upload_buffer_, &begin);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, layers - 1};
    dfn.vkCmdPipelineBarrier(upload_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &barrier);
    dfn.vkEndCommandBuffer(upload_buffer_);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &upload_buffer_;
    dfn.vkResetFences(device, 1, &upload_fence_);
    {
      auto queue = device_->AcquireQueue(device_->queue_family_graphics_compute(), 0);
      dfn.vkQueueSubmit(queue.queue(), 1, &submit, upload_fence_);
    }
    dfn.vkWaitForFences(device, 1, &upload_fence_, VK_TRUE, UINT64_MAX);
  }

  // --- Setup -----------------------------------------------------------------

  VkFramebuffer GetOutputFramebuffer(VkImageView view) {
    auto it = output_framebuffers_.find(view);
    if (it != output_framebuffers_.end()) return it->second;
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = blit_render_pass_;
    info.attachmentCount = 1;
    info.pAttachments = &view;
    info.width = kBackBufferWidth;
    info.height = kBackBufferHeight;
    info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (device_->functions().vkCreateFramebuffer(device_->device(), &info, nullptr,
                                                 &framebuffer) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    output_framebuffers_.emplace(view, framebuffer);
    return framebuffer;
  }

  // with_depth adds a kDepthFormat attachment (1), loaded or cleared like the
  // color one and kept in DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
  VkRenderPass CreateRenderPass(VkFormat format, VkAttachmentLoadOp load_op,
                                VkImageLayout initial_layout, VkImageLayout final_layout,
                                VkPipelineStageFlags release_stage, VkAccessFlags release_access,
                                bool with_depth = false) {
    std::array<VkAttachmentDescription, 2> attachments{};
    VkAttachmentDescription& attachment = attachments[0];
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = load_op;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = initial_layout;
    attachment.finalLayout = final_layout;
    VkAttachmentDescription& depth = attachments[1];
    depth.format = kDepthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = depth.stencilLoadOp = load_op;
    depth.storeOp = depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.initialLayout = load_op == VK_ATTACHMENT_LOAD_OP_LOAD
                              ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    if (with_depth) subpass.pDepthStencilAttachment = &depth_ref;
    constexpr VkPipelineStageFlags kDepthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkAccessFlags kDepthAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = release_stage;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = release_access;
    if (with_depth) {
      // Depth: previous passes' tests and depth resolves (transfer) before
      // this pass's tests, and this pass's tests before later ones.
      dependencies[0].srcStageMask |= kDepthStages | VK_PIPELINE_STAGE_TRANSFER_BIT;
      dependencies[0].dstStageMask |= kDepthStages;
      dependencies[0].srcAccessMask |= kDepthAccess;
      dependencies[0].dstAccessMask |= kDepthAccess;
      dependencies[1].srcStageMask |= kDepthStages;
      dependencies[1].dstStageMask |= kDepthStages;
      dependencies[1].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      dependencies[1].dstAccessMask |= kDepthAccess;
    }
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = with_depth ? 2 : 1;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = uint32_t(dependencies.size());
    info.pDependencies = dependencies.data();
    VkRenderPass render_pass = VK_NULL_HANDLE;
    device_->functions().vkCreateRenderPass(device_->device(), &info, nullptr, &render_pass);
    return render_pass;
  }

  // An anti-aliased target's pass (section 20.12): multisampled color and
  // depth (attachments 0 and 1, loaded or cleared, kept in attachment
  // layouts) resolved at the end of the pass into the target's single-sampled
  // image (2, then SHADER_READ_ONLY_OPTIMAL like other targets) and depth
  // buffer (3, sample 0 of depth and stencil). The render area is always the
  // whole target, so the resolves overwrite all of 2 and 3.
  VkRenderPass CreateMsaaRenderPass(VkFormat format, VkSampleCountFlagBits samples,
                                    VkAttachmentLoadOp load_op) {
    const bool load = load_op == VK_ATTACHMENT_LOAD_OP_LOAD;
    std::array<VkAttachmentDescription2, 4> attachments{};
    for (auto& attachment : attachments) {
      attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    attachments[0].format = format;
    attachments[0].samples = samples;
    attachments[0].loadOp = load_op;
    attachments[0].initialLayout =
        load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].format = kDepthFormat;
    attachments[1].samples = samples;
    attachments[1].loadOp = attachments[1].stencilLoadOp = load_op;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].initialLayout =
        load ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[2].format = format;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[3].format = kDepthFormat;
    attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[3].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    constexpr VkImageAspectFlags kDepthAspects =
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    const VkAttachmentReference2 color_ref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0,
                                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                           VK_IMAGE_ASPECT_COLOR_BIT};
    const VkAttachmentReference2 depth_ref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1,
                                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                           kDepthAspects};
    const VkAttachmentReference2 color_resolve_ref{
        VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 2,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT};
    const VkAttachmentReference2 depth_resolve_ref{
        VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 3,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kDepthAspects};
    VkSubpassDescriptionDepthStencilResolve depth_resolve{
        VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    depth_resolve.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    depth_resolve.stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    depth_resolve.pDepthStencilResolveAttachment = &depth_resolve_ref;
    VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    subpass.pNext = &depth_resolve;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pResolveAttachments = &color_resolve_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    // As CreateRenderPass with depth; resolves run in the color attachment
    // output stage, so it is in both directions.
    constexpr VkPipelineStageFlags kStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkAccessFlags kAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    std::array<VkSubpassDependency2, 2> dependencies{};
    for (auto& dependency : dependencies) {
      dependency.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    }
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | kStages;
    dependencies[0].dstStageMask = kStages;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | kAccess;
    dependencies[0].dstAccessMask = kAccess;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = kStages;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | kStages;
    dependencies[1].srcAccessMask = kAccess;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | kAccess;
    VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    info.attachmentCount = uint32_t(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = uint32_t(dependencies.size());
    info.pDependencies = dependencies.data();
    VkRenderPass render_pass = VK_NULL_HANDLE;
    create_render_pass2_(device_->device(), &info, nullptr, &render_pass);
    return render_pass;
  }

  bool CreateObjects() {
    const auto& dfn = device_->functions();
    const VkDevice device = device_->device();

    // vkCmdCopyImage and vkCmdBlitImage (core 1.0) are not in the SDK's
    // device function table.
    const auto get_proc = device_->vulkan_instance()->functions().vkGetDeviceProcAddr;
    cmd_copy_image_ = reinterpret_cast<PFN_vkCmdCopyImage>(get_proc(device, "vkCmdCopyImage"));
    cmd_blit_image_ = reinterpret_cast<PFN_vkCmdBlitImage>(get_proc(device, "vkCmdBlitImage"));
    if (!cmd_copy_image_ || !cmd_blit_image_) return false;
    // Multisampled targets resolve their depth in the render pass, which
    // needs render pass 2 (core 1.2, or VK_KHR_create_renderpass2).
    create_render_pass2_ =
        reinterpret_cast<PFN_vkCreateRenderPass2>(get_proc(device, "vkCreateRenderPass2"));
    if (!create_render_pass2_) {
      create_render_pass2_ =
          reinterpret_cast<PFN_vkCreateRenderPass2>(get_proc(device, "vkCreateRenderPass2KHR"));
    }
    if (!create_render_pass2_) REXGPU_WARN("[native] no vkCreateRenderPass2: MSAA is off");

    // Command pools, buffers and fences: one for frames, one for uploads.
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device_->queue_family_graphics_compute();
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &command_pool_) != VK_SUCCESS ||
        dfn.vkCreateCommandPool(device, &pool_info, nullptr, &upload_pool_) != VK_SUCCESS) {
      return false;
    }
    alloc.commandPool = command_pool_;
    if (dfn.vkAllocateCommandBuffers(device, &alloc, &command_buffer_) != VK_SUCCESS) return false;
    alloc.commandPool = upload_pool_;
    if (dfn.vkAllocateCommandBuffers(device, &alloc, &upload_buffer_) != VK_SUCCESS ||
        dfn.vkCreateFence(device, &fence_info, nullptr, &fence_) != VK_SUCCESS ||
        dfn.vkCreateFence(device, &fence_info, nullptr, &upload_fence_) != VK_SUCCESS) {
      return false;
    }

    // Upload ring (vertices, indices, constants), persistently mapped.
    if (!vk_ui::util::CreateDedicatedAllocationBuffer(
            device_, kUploadRingSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            vk_ui::util::MemoryPurpose::kUpload, ring_buffer_, ring_memory_, &ring_memory_type_,
            &ring_memory_size_)) {
      return false;
    }
    void* mapping = nullptr;
    if (dfn.vkMapMemory(device, ring_memory_, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
      return false;
    }
    ring_mapping_ = static_cast<uint8_t*>(mapping);

    // Vertex stream cache (section 20.15). Without it every stream goes
    // through the ring.
    if (REXCVAR_GET(wwe2k15_native_stream_cache) &&
        vk_ui::util::CreateDedicatedAllocationBuffer(
            device_, kStreamCacheSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            vk_ui::util::MemoryPurpose::kUpload, stream_cache_buffer_, stream_cache_memory_,
            &stream_cache_memory_type_, &stream_cache_memory_size_)) {
      void* cache_mapping = nullptr;
      if (dfn.vkMapMemory(device, stream_cache_memory_, 0, VK_WHOLE_SIZE, 0, &cache_mapping) ==
          VK_SUCCESS) {
        stream_cache_mapping_ = static_cast<uint8_t*>(cache_mapping);
      }
    }
    if (!stream_cache_mapping_) REXGPU_WARN("[native] no vertex stream cache");

    // Back buffer and its render pass (draws), and the blit render pass.
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kBackBufferFormat;
    image_info.extent = {kBackBufferWidth, kBackBufferHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!vk_ui::util::CreateDedicatedAllocationImage(device_, image_info,
                                                     vk_ui::util::MemoryPurpose::kDeviceLocal,
                                                     back_buffer_.image, back_buffer_.memory)) {
      return false;
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = back_buffer_.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kBackBufferFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &back_buffer_.view) != VK_SUCCESS) {
      return false;
    }
    blit_render_pass_ = CreateRenderPass(
        vk_ui::VulkanPresenter::kGuestOutputFormat, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        vk_ui::VulkanPresenter::kGuestOutputInternalLayout,
        vk_ui::VulkanPresenter::kGuestOutputInternalStageMask,
        vk_ui::VulkanPresenter::kGuestOutputInternalAccessMask);
    if (!GetPasses(kBackBufferFormat) || !blit_render_pass_) return false;
    const Texture back_buffer_depth = CreateDepthImage(kBackBufferWidth, kBackBufferHeight);
    if (back_buffer_depth.view == VK_NULL_HANDLE) return false;
    back_buffer_framebuffer_ = CreateFramebuffer(back_buffer_.view, back_buffer_depth.view,
                                                 kBackBufferWidth, kBackBufferHeight);
    if (back_buffer_framebuffer_ == VK_NULL_HANDLE) return false;
    // Depth resolves: the largest rect is a whole target.
    if (!vk_ui::util::CreateDedicatedAllocationBuffer(
            device_, VkDeviceSize(kMaxTargetWidth) * kMaxTargetHeight * sizeof(float),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            vk_ui::util::MemoryPurpose::kDeviceLocal, depth_copy_buffer_, depth_copy_memory_)) {
      return false;
    }
    // The back buffer is EDRAM base 0 with a 1280-pixel pitch (section 20.5).
    RenderTarget back_buffer_target;
    back_buffer_target.image = back_buffer_;
    back_buffer_target.depth = back_buffer_depth;
    back_buffer_target.framebuffer = back_buffer_framebuffer_;
    back_buffer_target.width = kBackBufferWidth;
    back_buffer_target.height = kBackBufferHeight;
    back_buffer_target.is_back_buffer = true;
    back_buffer_target_ =
        &render_targets_.emplace(kBackBufferWidth << 12, back_buffer_target).first->second;

    // Samplers.
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &linear_clamp_sampler_) != VK_SUCCESS) {
      return false;
    }
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &linear_wrap_sampler_) != VK_SUCCESS) {
      return false;
    }

    // Descriptor set layouts for the game shaders (section 19.3).
    std::array<VkDescriptorSetLayoutBinding, 3> constant_bindings{};
    for (uint32_t i = 0; i < 3; ++i) {
      constant_bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    std::array<VkDescriptorSetLayoutBinding, 4> texture_bindings{};
    for (uint32_t i = 0; i < 4; ++i) {
      texture_bindings[i] = {i, i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                       : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                             kFetchSlots, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                             nullptr};
    }
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = uint32_t(constant_bindings.size());
    layout_info.pBindings = constant_bindings.data();
    if (dfn.vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &constants_set_layout_) !=
        VK_SUCCESS) {
      return false;
    }
    layout_info.bindingCount = uint32_t(texture_bindings.size());
    layout_info.pBindings = texture_bindings.data();
    if (dfn.vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &texture_set_layout_) !=
        VK_SUCCESS) {
      return false;
    }
    std::array<VkDescriptorSetLayout, 2> set_layouts{constants_set_layout_, texture_set_layout_};
    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = uint32_t(set_layouts.size());
    pipeline_layout_info.pSetLayouts = set_layouts.data();
    if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout_) !=
        VK_SUCCESS) {
      return false;
    }

    // Descriptor pools: a static one (constants set, blit set) and a per-frame
    // one for texture sets.
    std::array<VkDescriptorPoolSize, 3> static_sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 4},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1}}};
    VkDescriptorPoolCreateInfo pool_create{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_create.maxSets = 2;
    pool_create.poolSizeCount = uint32_t(static_sizes.size());
    pool_create.pPoolSizes = static_sizes.data();
    if (dfn.vkCreateDescriptorPool(device, &pool_create, nullptr, &static_descriptor_pool_) !=
        VK_SUCCESS) {
      return false;
    }
    constexpr uint32_t kFrameSets = 4096;
    std::array<VkDescriptorPoolSize, 2> frame_sizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFrameSets * kFetchSlots * 3},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kFrameSets * kFetchSlots}}};
    pool_create.maxSets = kFrameSets;
    pool_create.poolSizeCount = uint32_t(frame_sizes.size());
    pool_create.pPoolSizes = frame_sizes.data();
    if (dfn.vkCreateDescriptorPool(device, &pool_create, nullptr, &frame_descriptor_pool_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_alloc.descriptorPool = static_descriptor_pool_;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &constants_set_layout_;
    if (dfn.vkAllocateDescriptorSets(device, &set_alloc, &constants_set_) != VK_SUCCESS) {
      return false;
    }
    std::array<VkDescriptorBufferInfo, 3> buffer_infos{{
        {ring_buffer_, 0, kConstantsRange},
        {ring_buffer_, 0, kConstantsRange},
        {ring_buffer_, 0, kSharedConstantsRange}}};
    std::array<VkWriteDescriptorSet, 3> constant_writes{};
    for (uint32_t i = 0; i < 3; ++i) {
      constant_writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, constants_set_, i, 0,
                            1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, nullptr,
                            &buffer_infos[i], nullptr};
    }
    dfn.vkUpdateDescriptorSets(device, uint32_t(constant_writes.size()), constant_writes.data(),
                               0, nullptr);

    // Placeholder textures for empty slots.
    if (!CreateDummy(dummy_2d_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0) ||
        !CreateDummy(dummy_3d_, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0) ||
        !CreateDummy(dummy_cube_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6,
                     VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) {
      return false;
    }

    return CreateBlitPipeline();
  }

  bool CreateBlitPipeline() {
    const auto& dfn = device_->functions();
    const VkDevice device = device_->device();
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr};
    VkDescriptorSetLayoutCreateInfo set_layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout_info.bindingCount = uint32_t(bindings.size());
    set_layout_info.pBindings = bindings.data();
    if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &blit_set_layout_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_alloc.descriptorPool = static_descriptor_pool_;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &blit_set_layout_;
    if (dfn.vkAllocateDescriptorSets(device, &set_alloc, &blit_set_) != VK_SUCCESS) return false;
    VkDescriptorImageInfo image_descriptor{VK_NULL_HANDLE, back_buffer_.view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sampler_descriptor{linear_clamp_sampler_, VK_NULL_HANDLE,
                                             VK_IMAGE_LAYOUT_UNDEFINED};
    VkDescriptorBufferInfo gamma_descriptor{ring_buffer_, 0, kGammaRampSize};
    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, blit_set_, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image_descriptor, nullptr, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, blit_set_, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_SAMPLER, &sampler_descriptor, nullptr, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, blit_set_, 2, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, nullptr, &gamma_descriptor, nullptr};
    dfn.vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &blit_set_layout_;
    if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &blit_layout_) != VK_SUCCESS) {
      return false;
    }
    VkShaderModule vs = vk_ui::util::CreateShaderModule(device_, kPresentBlitVS,
                                                        sizeof(kPresentBlitVS));
    VkShaderModule ps = vk_ui::util::CreateShaderModule(device_, kPresentBlitPS,
                                                        sizeof(kPresentBlitPS));
    if (vs == VK_NULL_HANDLE || ps == VK_NULL_HANDLE) return false;
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vs, "VSMain", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, ps, "PSMain", nullptr};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = uint32_t(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = blit_layout_;
    info.renderPass = blit_render_pass_;
    const VkResult result =
        dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &blit_pipeline_);
    dfn.vkDestroyShaderModule(device, vs, nullptr);
    dfn.vkDestroyShaderModule(device, ps, nullptr);
    return result == VK_SUCCESS;
  }

  bool initialized_ = false;
  const vk_ui::VulkanDevice* device_ = nullptr;
  rex::ui::Presenter* presenter_ = nullptr;
  std::string shader_dir_;

  VkCommandPool command_pool_ = VK_NULL_HANDLE, upload_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE, upload_buffer_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE, upload_fence_ = VK_NULL_HANDLE;
  bool frame_open_ = false;

  VkBuffer ring_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory ring_memory_ = VK_NULL_HANDLE;
  uint8_t* ring_mapping_ = nullptr;
  VkDeviceSize ring_offset_ = 0;
  uint32_t ring_memory_type_ = 0;
  VkDeviceSize ring_memory_size_ = 0;

  // Vertex stream cache (section 20.15), persistently mapped like the ring.
  VkBuffer stream_cache_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory stream_cache_memory_ = VK_NULL_HANDLE;
  uint8_t* stream_cache_mapping_ = nullptr;
  uint32_t stream_cache_memory_type_ = 0;
  VkDeviceSize stream_cache_memory_size_ = 0;
  VkDeviceSize stream_cache_offset_ = 0;  // bump allocation, reset by a flush
  VkDeviceSize stream_cache_dirty_begin_ = kStreamCacheSize, stream_cache_dirty_end_ = 0;
  bool stream_cache_flush_ = false;
  std::unordered_map<uint64_t, CachedStream> stream_cache_;
  uint64_t cached_bytes_ = 0;  // vertex bytes drawn from the cache, this report
  uint32_t stream_cache_flushes_ = 0;

  Texture back_buffer_;
  VkFramebuffer back_buffer_framebuffer_ = VK_NULL_HANDLE;
  // Depth resolves go through this buffer (depth to R32_SFLOAT, section 20.10).
  VkBuffer depth_copy_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory depth_copy_memory_ = VK_NULL_HANDLE;
  VkRenderPass blit_render_pass_ = VK_NULL_HANDLE;
  // By color format (low 32 bits) and sample count (high 32 bits).
  std::unordered_map<uint64_t, TargetPasses> target_passes_;
  PFN_vkCreateRenderPass2 create_render_pass2_ = nullptr;  // null: no MSAA
  PFN_vkCmdCopyImage cmd_copy_image_ = nullptr;
  PFN_vkCmdBlitImage cmd_blit_image_ = nullptr;
  std::unordered_set<uint32_t> unsupported_formats_;  // logged once each
  // By TargetKey; std::unordered_map keeps element addresses stable.
  std::unordered_map<uint32_t, RenderTarget> render_targets_;
  RenderTarget* back_buffer_target_ = nullptr;
  RenderTarget* current_target_ = nullptr;  // the open render pass, if any
  bool back_buffer_cleared_ = false;
  // Resolve destinations by texture base address (fetch constant, >> 12).
  std::unordered_map<uint32_t, ResolvedTexture> resolved_;
  std::vector<Texture> retired_;  // replaced images, freed at the next frame start
  uint32_t resolves_ = 0;
  VkSampler linear_clamp_sampler_ = VK_NULL_HANDLE, linear_wrap_sampler_ = VK_NULL_HANDLE;
  std::unordered_map<uint32_t, VkSampler> samplers_;  // GetSampler, by addressing and filters

  VkDescriptorSetLayout constants_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout texture_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool static_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorPool frame_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet constants_set_ = VK_NULL_HANDLE;
  Texture dummy_2d_, dummy_3d_, dummy_cube_;

  VkDescriptorSetLayout blit_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet blit_set_ = VK_NULL_HANDLE;
  VkPipelineLayout blit_layout_ = VK_NULL_HANDLE;
  VkPipeline blit_pipeline_ = VK_NULL_HANDLE;
  std::unordered_map<VkImageView, VkFramebuffer> output_framebuffers_;

  std::unordered_map<uint64_t, VkShaderModule> shaders_;
  std::unordered_map<uint64_t, bool> missing_shaders_;
  std::unordered_map<uint64_t, ShaderInputs> shader_inputs_;
  std::unordered_map<uint64_t, Pipeline> pipelines_;
  std::unordered_map<uint64_t, CachedTexture> textures_;

  uint32_t frame_draws_ = 0, drawn_ = 0, skipped_primitive_ = 0, skipped_pipeline_ = 0;
  uint32_t ring_overflows_ = 0, descriptor_overflows_ = 0;
  uint32_t mipped_textures_ = 0;  // created with more than one level, ever
  uint64_t vertex_bytes_ = 0;
  std::unordered_map<uint32_t, bool> frame_streams_;
  std::chrono::steady_clock::time_point last_report_, frame_start_;
  std::chrono::steady_clock::duration draw_time_{}, texture_create_time_{}, fence_wait_time_{};
  // Draw time of every frame since the last report, for its average.
  std::chrono::steady_clock::duration report_draw_time_{};
  uint32_t report_frames_ = 0;
  uint32_t textures_created_ = 0;
  std::unordered_map<uint64_t, VkImageView> frame_textures_;  // this frame's lookups
  uint64_t recording_frame_ = 0;  // the frame the next Present presents
  std::vector<PendingDump> pending_dumps_;
};

VulkanCore& Core() {
  static VulkanCore core;
  return core;
}

std::mutex g_core_mutex;

}  // namespace

void NativeDraw(const NativeDrawState& state) {
  if (!REXCVAR_GET(wwe2k15_native_renderer)) return;
  std::lock_guard lock(g_core_mutex);
  if (Core().EnsureInitialized()) Core().Draw(state);
}

void NativeResolve(uint32_t surface_info, uint32_t color_info, bool depth,
                   const std::array<uint32_t, 6>& dest_fetch, bool has_rect, GuestRect rect,
                   const ClearValues& clear) {
  if (!REXCVAR_GET(wwe2k15_native_renderer)) return;
  std::lock_guard lock(g_core_mutex);
  if (Core().EnsureInitialized()) {
    Core().Resolve(surface_info, color_info, depth, dest_fetch, has_rect, rect, clear);
  }
}

void NativeClear(uint32_t surface_info, uint32_t color_info, const ClearValues& clear,
                 const std::vector<GuestRect>& rects) {
  if (!REXCVAR_GET(wwe2k15_native_renderer)) return;
  std::lock_guard lock(g_core_mutex);
  if (Core().EnsureInitialized()) Core().Clear(surface_info, color_info, clear, rects);
}

// Called from the Present hook (renderer_hooks.cpp) on the guest thread.
std::atomic<uint64_t> g_last_frame{0};

uint64_t NativeLastFrame() { return g_last_frame.load(std::memory_order_relaxed); }

void NativeOnPresent(uint64_t frame) {
  g_last_frame.store(frame, std::memory_order_relaxed);
  if (!REXCVAR_GET(wwe2k15_native_renderer)) return;
  std::lock_guard lock(g_core_mutex);
  if (Core().EnsureInitialized()) Core().Present(frame);
}

}  // namespace wwe2k15::native
