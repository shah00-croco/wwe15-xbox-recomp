// Native renderer, game side (docs/native-renderer.md, section 19 / B3).
//
// The game's Xbox D3D library is statically linked; its functions are hooked
// here (addresses and evidence: section 17). This file tracks the resources the
// game creates, keyed by their guest object address, so that the renderer can
// convert each one once instead of emulating the Xenos GPU. All hooks call the
// original function, so the game behaves as before in every GPU plugin.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <algorithm>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/graphics/xenos.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "generated/default/wwe2k15_pch.h"
#include "native_draw.h"

REXCVAR_DECLARE(bool, wwe2k15_native_renderer);

namespace wwe2k15::native {
inline bool NativeRendererEnabled() { return REXCVAR_GET(wwe2k15_native_renderer); }
}  // namespace wwe2k15::native

namespace wwe2k15::native {

namespace xenos = rex::graphics::xenos;

enum class ResourceKind : uint8_t { kTexture, kVertexBuffer, kIndexBuffer, kVertexShader,
                                    kPixelShader, kVertexDeclaration };

struct TrackedResource {
  ResourceKind kind;
  uint32_t guest_object = 0;
  // Textures: the fetch constant from the texture header (host byte order).
  xenos::xe_gpu_texture_fetch_t fetch{};
  // Buffers: data address and length in bytes.
  uint32_t data_address = 0;
  uint32_t length = 0;
  // Shaders: hash of the compiled shader container (matches the dump files).
  uint64_t container_hash = 0;
  // Vertex declarations: the D3DVERTEXELEMENT9 entries (12 bytes each).
  std::vector<uint8_t> elements;
};

class ResourceRegistry {
 public:
  void Add(TrackedResource resource) {
    std::lock_guard lock(mutex_);
    ++created_[size_t(resource.kind)];
    resources_[resource.guest_object] = std::move(resource);
  }

  const TrackedResource* Find(uint32_t guest_object) const {
    std::lock_guard lock(mutex_);
    auto it = resources_.find(guest_object);
    return it == resources_.end() ? nullptr : &it->second;
  }

  void LogSummary() {
    std::lock_guard lock(mutex_);
    std::unordered_map<std::string, uint32_t> texture_kinds;
    for (const auto& [address, resource] : resources_) {
      if (resource.kind == ResourceKind::kTexture) {
        const auto& f = resource.fetch;
        ++texture_kinds[fmt::format("fmt{}{}{}", uint32_t(f.format), f.tiled ? "/tiled" : "",
                                    f.dimension == xenos::DataDimension::k2DOrStacked ? ""
                                                                                      : "/not2d")];
      }
    }
    std::string textures;
    for (const auto& [kind, count] : texture_kinds) {
      textures += fmt::format(" {}x{}", kind, count);
    }
    REXGPU_INFO("[native] resources: {} textures, {} vertex buffers, {} index buffers, "
                "{} vertex shaders, {} pixel shaders, {} declarations (created so far); "
                "live textures by format:{}",
                created_[0], created_[1], created_[2], created_[3], created_[4], created_[5],
                textures);
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, TrackedResource> resources_;
  uint32_t created_[6]{};
};

ResourceRegistry& Registry() {
  static ResourceRegistry registry;
  return registry;
}

namespace {

uint32_t LoadBe32(uint8_t* base, uint32_t address) { return REX_LOAD_U32(address); }

uint64_t HashGuestBytes(uint8_t* base, uint32_t address, uint32_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < size; ++i) {
    hash = (hash ^ base[address + i]) * 0x100000001B3ull;
  }
  return hash;
}

}  // namespace

void LogResourceSummary() { Registry().LogSummary(); }

}  // namespace wwe2k15::native

using namespace wwe2k15::native;

// CreateTexture (section 12.1 / 17.3): returns the texture object; its header
// holds the texture fetch constant at +0x1C..+0x30 (as SetTexture copies it).
REX_EXTERN(__imp__sub_82B9AFA8);
REX_HOOK_RAW(sub_82B9AFA8) {
  __imp__sub_82B9AFA8(ctx, base);
  if (const uint32_t object = ctx.r3.u32) {
    TrackedResource resource{ResourceKind::kTexture, object};
    auto* words = reinterpret_cast<uint32_t*>(&resource.fetch);
    for (uint32_t i = 0; i < 6; ++i) {
      words[i] = LoadBe32(base, object + 0x1C + i * 4);
    }
    Registry().Add(std::move(resource));
  }
}

// CreateVertexBuffer / CreateIndexBuffer (17.5): r3 = length; the object's
// data address is at +0x18.
#define TRACK_BUFFER(addr, kind)                                       \
  REX_EXTERN(__imp__sub_##addr);                                       \
  REX_HOOK_RAW(sub_##addr) {                                           \
    const uint32_t length = ctx.r3.u32;                                \
    __imp__sub_##addr(ctx, base);                                      \
    if (const uint32_t object = ctx.r3.u32) {                          \
      TrackedResource resource{kind, object};                          \
      resource.data_address = LoadBe32(base, object + 0x18);           \
      resource.length = length;                                        \
      Registry().Add(std::move(resource));                             \
    }                                                                  \
  }
TRACK_BUFFER(82B9F570, ResourceKind::kVertexBuffer)
TRACK_BUFFER(82B9F648, ResourceKind::kIndexBuffer)

// CreateVertexShader / CreatePixelShader (17.5): r3 = the compiled shader
// container (flags, virtualSize, physicalSize, ...), hashed like the dumps.
#define TRACK_SHADER(addr, kind)                                                    \
  REX_EXTERN(__imp__sub_##addr);                                                    \
  REX_HOOK_RAW(sub_##addr) {                                                        \
    const uint32_t container = ctx.r3.u32;                                          \
    const uint32_t size = LoadBe32(base, container + 4) + LoadBe32(base, container + 8); \
    const uint64_t hash = size < 0x100000 ? HashGuestBytes(base, container, size) : 0; \
    __imp__sub_##addr(ctx, base);                                                   \
    if (const uint32_t object = ctx.r3.u32) {                                       \
      static std::atomic<uint32_t> logged{0};                                       \
      if (logged.fetch_add(1) < 3) {                                                \
        REXGPU_INFO("[native] shader container {:08X} size {} hash {:016X}",        \
                    container, size, hash);                                         \
      }                                                                             \
      TrackedResource resource{kind, object};                                       \
      resource.container_hash = hash;                                               \
      Registry().Add(std::move(resource));                                          \
    }                                                                               \
  }
TRACK_SHADER(82BA39D0, ResourceKind::kVertexShader)
TRACK_SHADER(82BA3BB8, ResourceKind::kPixelShader)

// Inline-vertex draws (section 10.3): BeginVertices(device, prim, count,
// stride) returns where the game writes the vertices; EndVertices(device)
// closes the draw. The draw is complete at EndVertices.
REXCVAR_DEFINE_INT32(wwe2k15_native_dump_draws, 0, "Native",
                     "Log the device state of the first N inline-vertex draws");
REXCVAR_DEFINE_INT32(wwe2k15_native_dump_frame, 0, "Native",
                     "Log the device state and all vertices of every inline-vertex draw "
                     "of this frame (0: off)");

namespace {
struct PendingInlineDraw {
  uint32_t device = 0, prim = 0, count = 0, stride = 0, data = 0;
  bool active = false;
};
thread_local PendingInlineDraw g_inline;
std::atomic<int32_t> g_dumped_draws{0};

void DumpInlineDraw(uint8_t* base, const PendingInlineDraw& draw, bool all_vertices) {
  const uint32_t dev = draw.device;
  const uint32_t decl = REX_LOAD_U32(dev + 0x2FD0);
  const uint32_t vs = REX_LOAD_U32(dev + 0x3310);
  const uint32_t ps = REX_LOAD_U32(dev + 0x330C);
  const auto* decl_resource = Registry().Find(decl);
  const auto* vs_resource = Registry().Find(vs);
  const auto* ps_resource = Registry().Find(ps);
  std::string elements;
  if (decl_resource) {
    for (size_t i = 0; i + 12 <= decl_resource->elements.size(); i += 12) {
      const uint8_t* e = decl_resource->elements.data() + i;
      const uint32_t stream = (e[0] << 8) | e[1];
      if (stream == 0xFF) break;
      elements += fmt::format(" [s{} off{} type{:08X} usage{}.{}]", stream, (e[2] << 8) | e[3],
                              (uint32_t(e[4]) << 24) | (e[5] << 16) | (e[6] << 8) | e[7],
                              e[9], e[10]);
    }
  }
  REXGPU_INFO("[native] inline draw: prim {} count {} stride {} data {:08X} decl {:08X}{}{} "
              "vs {:08X}={:016X} ps {:08X}={:016X}",
              draw.prim, draw.count, draw.stride, draw.data, decl,
              decl_resource ? "" : "(unknown)", elements, vs,
              vs_resource ? vs_resource->container_hash : 0, ps,
              ps_resource ? ps_resource->container_hash : 0);
  for (uint32_t slot = 0; slot < 4; ++slot) {
    const uint32_t fc = dev + 0x480 + slot * 0x18;
    xenos::xe_gpu_texture_fetch_t fetch;
    auto* words = reinterpret_cast<uint32_t*>(&fetch);
    for (uint32_t i = 0; i < 6; ++i) words[i] = REX_LOAD_U32(fc + i * 4);
    if (fetch.type == xenos::FetchConstantType::kTexture ||
        fetch.type == xenos::FetchConstantType::kInvalidTexture) {
      REXGPU_INFO("[native]   tex slot {}: fmt {} {}x{} tiled {} base {:08X} endian {}", slot,
                  uint32_t(fetch.format), uint32_t(fetch.size_2d.width) + 1,
                  uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.tiled),
                  uint32_t(fetch.base_address) << 12, uint32_t(fetch.endianness));
    }
  }
  const uint32_t vertex_count = all_vertices ? std::min<uint32_t>(draw.count, 64) : 1;
  for (uint32_t v = 0; v < vertex_count; ++v) {
    // Float words as values (positions, texcoords); others in hex.
    std::string verts;
    for (uint32_t i = 0; i < std::min<uint32_t>(draw.stride, 32) / 4; ++i) {
      const uint32_t word = REX_LOAD_U32(draw.data + v * draw.stride + i * 4);
      float value;
      std::memcpy(&value, &word, 4);
      verts += std::isfinite(value) && std::abs(value) < 1e6f && std::abs(value) > 1e-6f
                   ? fmt::format(" {:g}", value)
                   : fmt::format(" {:08X}", word);
    }
    REXGPU_INFO("[native]   vertex {}:{}", v, verts);
  }
}
}  // namespace

REX_EXTERN(__imp__sub_82BA3D08);
REX_HOOK_RAW(sub_82BA3D08) {
  PendingInlineDraw draw{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  __imp__sub_82BA3D08(ctx, base);
  draw.data = ctx.r3.u32;
  draw.active = true;
  g_inline = draw;
}

namespace {
// Research (section 20.5): logs the render-target registers from the shadow
// block 0x2000 (+0x2880), the viewport (PA_CL_VPORT_*, 0x210F-0x2114) and
// PA_CL_VTE_CNTL / RB_MODECONTROL whenever the render target changes.
// Logs each new combination once.
bool FirstTime(uint64_t key) {
  static std::mutex mutex;
  static std::unordered_map<uint64_t, bool> seen;
  std::lock_guard lock(mutex);
  return seen.emplace(key, true).second && seen.size() < 200;
}

void LogRenderTargetChange(uint8_t* base, uint32_t dev) {
  const uint32_t surface_info = REX_LOAD_U32(dev + 0x2880);
  const uint32_t color_info = REX_LOAD_U32(dev + 0x2884);
  const uint32_t xscale = REX_LOAD_U32(dev + 0x28CC + (0x210F - 0x2100) * 4);
  if (!FirstTime((uint64_t(surface_info) << 32) ^ (uint64_t(color_info) << 16) ^ xscale)) return;
  float vport[6];
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t bits = REX_LOAD_U32(dev + 0x28CC + (0x210F - 0x2100 + i) * 4);
    std::memcpy(&vport[i], &bits, 4);
  }
  REXGPU_INFO("[native] render target: surface_info {:08X} color_info {:08X} depth_info {:08X} "
              "vport xs {} xo {} ys {} yo {} zs {} zo {} vte {:08X} mode {:08X}",
              surface_info, color_info, REX_LOAD_U32(dev + 0x2888), vport[0], vport[1],
              vport[2], vport[3], vport[4], vport[5], REX_LOAD_U32(dev + 0x294C),
              REX_LOAD_U32(dev + 0x2954));
}
// For --wwe2k15_native_dump_frame: the target, viewport and texture sampling
// state of one draw.
void LogDrawTarget(uint8_t* base, uint32_t dev) {
  float vport[4];
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t bits = REX_LOAD_U32(dev + 0x28CC + (0x210F - 0x2100 + i) * 4);
    std::memcpy(&vport[i], &bits, 4);
  }
  REXGPU_INFO("[native]   target: surface_info {:08X} color_info {:08X} vport xs {} xo {} ys {} "
              "yo {} vte {:08X} blend {:08X}",
              REX_LOAD_U32(dev + 0x2880), REX_LOAD_U32(dev + 0x2884), vport[0], vport[1],
              vport[2], vport[3], REX_LOAD_U32(dev + 0x294C), REX_LOAD_U32(dev + 0x2938));
  for (uint32_t slot = 0; slot < 4; ++slot) {
    xenos::xe_gpu_texture_fetch_t fetch;
    auto* words = reinterpret_cast<uint32_t*>(&fetch);
    for (uint32_t i = 0; i < 6; ++i) words[i] = REX_LOAD_U32(dev + 0x480 + slot * 0x18 + i * 4);
    if (fetch.type != xenos::FetchConstantType::kTexture) continue;
    REXGPU_INFO("[native]   sampler slot {}: clamp {} {} mag {} min {} mip {}", slot,
                uint32_t(fetch.clamp_x), uint32_t(fetch.clamp_y), uint32_t(fetch.mag_filter),
                uint32_t(fetch.min_filter), uint32_t(fetch.mip_filter));
  }
}
}  // namespace

// D3DRESOLVE_DEPTHSTENCIL, D3DRESOLVE_CLEARRENDERTARGET, D3DRESOLVE_CLEARDEPTHSTENCIL.
constexpr uint32_t kResolveDepth = 0x4, kResolveClearColor = 0x100, kResolveClearDepth = 0x200;
// D3DCLEAR_TARGET0..3 are the low four bits, then D3DCLEAR_ZBUFFER and
// D3DCLEAR_STENCIL (the demo clears with 0x01, 0x0F, 0x10, 0x1F and 0x3F).
constexpr uint32_t kClearTargets = 0xF, kClearDepth = 0x10, kClearStencil = 0x20;

void wwe2k15::native::NativeOnResolve(uint8_t* base, uint32_t dev, uint32_t flags,
                                      uint32_t rect, uint32_t dest_texture, uint32_t clear_color,
                                      float clear_z, uint32_t clear_stencil) {
  if (!NativeRendererEnabled() || !dest_texture) return;
  std::array<uint32_t, 6> words;
  for (uint32_t i = 0; i < 6; ++i) words[i] = REX_LOAD_U32(dest_texture + 0x1C + i * 4);
  GuestRect source{};
  if (rect) {
    for (uint32_t i = 0; i < 4; ++i) source[i] = int32_t(REX_LOAD_U32(rect + i * 4));
  }
  const uint32_t surface_info = REX_LOAD_U32(dev + 0x2880);
  const uint32_t color_info = REX_LOAD_U32(dev + 0x2884);
  if (FirstTime((uint64_t(1) << 63) ^ (uint64_t(dest_texture) << 32) ^ color_info ^
                (uint64_t(flags) << 20))) {
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words.data(), sizeof(fetch));
    REXGPU_INFO("[native] resolve: flags {:08X} rect {:08X} ({} {} {} {}) dest {:08X} (fmt {} "
                "{}x{} tiled {} base {:08X} sign {}{}{}{} int {} exp {} swizzle {:03X} "
                "endian {}) "
                "surface_info {:08X} color_info {:08X}",
                flags, rect, source[0], source[1], source[2], source[3], dest_texture,
                uint32_t(fetch.format), uint32_t(fetch.size_2d.width) + 1,
                uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.tiled),
                uint32_t(fetch.base_address) << 12, uint32_t(fetch.sign_x),
                uint32_t(fetch.sign_y), uint32_t(fetch.sign_z), uint32_t(fetch.sign_w),
                uint32_t(fetch.num_format), int32_t(fetch.exp_adjust), uint32_t(fetch.swizzle),
                uint32_t(fetch.endianness), surface_info, color_info);
  }
  ClearValues clear;
  clear.color = (flags & kResolveClearColor) && clear_color;
  if (clear.color) {
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t bits = REX_LOAD_U32(clear_color + i * 4);
      std::memcpy(&clear.rgba[i], &bits, 4);
    }
  }
  clear.depth = clear.stencil = (flags & kResolveClearDepth) != 0;
  clear.z = clear_z;
  clear.stencil_value = clear_stencil;
  NativeResolve(surface_info, color_info, (flags & kResolveDepth) != 0, words, rect != 0, source,
                clear);
}

void wwe2k15::native::NativeOnClear(uint8_t* base, uint32_t dev, uint32_t count, uint32_t rects,
                                    uint32_t flags, uint32_t color, float z, uint32_t stencil) {
  if (!NativeRendererEnabled()) return;
  if (FirstTime((uint64_t(3) << 62) ^ flags)) {
    REXGPU_INFO("[native] clear: flags {:08X} color {:08X} z {} stencil {}", flags, color, z,
                stencil);
  }
  ClearValues clear;
  clear.color = (flags & kClearTargets) != 0;
  clear.depth = (flags & kClearDepth) != 0;
  clear.stencil = (flags & kClearStencil) != 0;
  if (!clear.color && !clear.depth && !clear.stencil) return;
  // D3DCOLOR: ARGB.
  clear.rgba = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
                float(color & 0xFF) / 255.0f, float(color >> 24) / 255.0f};
  clear.z = z;
  clear.stencil_value = stencil & 0xFF;
  std::vector<GuestRect> list;
  for (uint32_t r = 0; rects && r < std::min<uint32_t>(count, 64); ++r) {
    GuestRect rect;
    for (uint32_t i = 0; i < 4; ++i) rect[i] = int32_t(REX_LOAD_U32(rects + r * 16 + i * 4));
    list.push_back(rect);
  }
  NativeClear(REX_LOAD_U32(dev + 0x2880), REX_LOAD_U32(dev + 0x2884), clear, list);
}

namespace {
// Collects what the native Vulkan core needs for one draw from the D3D device
// object (fields: section 17.2 / 20.1).
void ReadDeviceState(uint8_t* base, uint32_t dev, NativeDrawState& state);

void SubmitInlineDraw(uint8_t* base, const PendingInlineDraw& draw) {
  NativeDrawState state;
  state.primitive = draw.prim;
  state.vertex_count = draw.count;
  auto& stream = state.streams[0];
  stream.stride = draw.stride;
  const uint32_t size = draw.count * draw.stride;
  if (!size || size > 1024 * 1024) return;
  stream.data.resize(size);
  for (uint32_t i = 0; i < size; i += 4) {
    const uint32_t word = REX_LOAD_U32(draw.data + i);
    stream.data[i] = uint8_t(word >> 24);
    stream.data[i + 1] = uint8_t(word >> 16);
    stream.data[i + 2] = uint8_t(word >> 8);
    stream.data[i + 3] = uint8_t(word);
  }
  ReadDeviceState(base, draw.device, state);
  NativeDraw(state);
}

// Shaders, declaration, constants, textures and render state of the device.
void ReadDeviceState(uint8_t* base, uint32_t dev, NativeDrawState& state) {
  const uint32_t decl = REX_LOAD_U32(dev + 0x2FD0);
  if (const auto* resource = Registry().Find(decl)) {
    state.declaration = resource->elements;
  }
  state.declaration_key = decl;
  if (const auto* vs = Registry().Find(REX_LOAD_U32(dev + 0x3310))) {
    state.vertex_shader = vs->container_hash;
  }
  if (const auto* ps = Registry().Find(REX_LOAD_U32(dev + 0x330C))) {
    state.pixel_shader = ps->container_hash;
  }
  for (uint32_t i = 0; i < 256 * 4; ++i) {
    state.vertex_constants[i] = REX_LOAD_U32(dev + 0x780 + i * 4);
    state.pixel_constants[i] = REX_LOAD_U32(dev + 0x1780 + i * 4);
  }
  for (uint32_t slot = 0; slot < 32; ++slot) {
    for (uint32_t i = 0; i < 6; ++i) {
      state.texture_fetch[slot][i] = REX_LOAD_U32(dev + 0x480 + slot * 0x18 + i * 4);
    }
  }
  // Boolean constants: SetVertexShaderConstantB (82BA2FF0) sets bit n % 32 of
  // dword 0x9E0 + n / 32 (+0x2780); pixel shader booleans follow from dword
  // 0x9E4 (+0x2790, b128 onwards as on the GPU). The loop constants follow
  // them, like SHADER_CONSTANT_LOOP_00-31 after the booleans:
  // SetVertexShaderConstantI (82BA30B0) writes i0-i15 to +0x27A0 and
  // SetPixelShaderConstantI (82BA3108) to +0x27E0 (loops 16-31), each as
  // count | start << 8 | step << 16 from the int4's x, y and z.
  state.vertex_bools = REX_LOAD_U32(dev + 0x2780);
  state.pixel_bools = REX_LOAD_U32(dev + 0x2790);
  for (uint32_t i = 0; i < 32; ++i) state.loop_constants[i] = REX_LOAD_U32(dev + 0x27A0 + i * 4);
  // Register shadow: block 0x2100 at +0x28CC, block 0x2200 at +0x2934.
  state.color_mask = REX_LOAD_U32(dev + 0x28CC + (0x2104 - 0x2100) * 4);
  state.alpha_ref = REX_LOAD_U32(dev + 0x28CC + (0x210E - 0x2100) * 4);
  state.depth_control = REX_LOAD_U32(dev + 0x2934);
  state.stencil_ref_mask = REX_LOAD_U32(dev + 0x28CC + (0x210D - 0x2100) * 4);
  state.stencil_ref_mask_bf = REX_LOAD_U32(dev + 0x28CC + (0x210C - 0x2100) * 4);
  state.blend_control = REX_LOAD_U32(dev + 0x2938);
  state.color_control = REX_LOAD_U32(dev + 0x293C);
  state.mode_control = REX_LOAD_U32(dev + 0x2948);
  state.surface_info = REX_LOAD_U32(dev + 0x2880);
  state.color_info = REX_LOAD_U32(dev + 0x2884);
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t bits = REX_LOAD_U32(dev + 0x28CC + (0x210F - 0x2100 + i) * 4);
    std::memcpy(&state.viewport[i], &bits, 4);
  }
  state.vte_control = REX_LOAD_U32(dev + 0x294C);
  LogRenderTargetChange(base, dev);
}
}  // namespace

// Indexed draws (82BA41C8, section 20.6): indices come from the current index
// buffer (+0x320C; data address at +0x18, 32-bit when bit 31 of word 0 is set),
// vertices from the vertex fetch constants SetStreamSource writes (stream s at
// +0x778 - s*8: physical address, then size in bytes) with the stride in dwords
// at +0x3268 + s.
void wwe2k15::native::NativeOnIndexedDraw(uint8_t* base, uint32_t dev, uint32_t primitive,
                                          uint32_t base_vertex, uint32_t start_index,
                                          uint32_t index_count) {
  if (!NativeRendererEnabled() || !index_count || index_count > (1u << 20)) return;
  const uint32_t index_buffer = REX_LOAD_U32(dev + 0x320C);
  if (!index_buffer) return;
  NativeDrawState state;
  state.primitive = primitive & 0x3F;
  state.vertex_count = index_count;
  state.base_vertex = int32_t(base_vertex);
  const bool index32 = (REX_LOAD_U32(index_buffer) & 0x80000000u) != 0;
  const uint32_t restart = index32 ? 0xFFFFFFFFu : 0xFFFFu;
  const uint32_t indices = REX_LOAD_U32(index_buffer + 0x18) + start_index * (index32 ? 4 : 2);
  state.indices.resize(index_count);
  uint32_t max_index = 0;
  for (uint32_t i = 0; i < index_count; ++i) {
    const uint32_t index =
        index32 ? REX_LOAD_U32(indices + i * 4) : uint32_t(REX_LOAD_U16(indices + i * 2));
    state.indices[i] = index;
    if (index != restart) max_index = std::max(max_index, index);
  }
  ReadDeviceState(base, dev, state);

  // Copy each stream the declaration uses, up to the highest vertex indexed.
  bool used[4]{};
  for (size_t i = 0; i + 12 <= state.declaration.size(); i += 12) {
    const uint32_t stream = (state.declaration[i] << 8) | state.declaration[i + 1];
    if (stream == 0xFF) break;
    if (stream < 4) used[stream] = true;
  }
  const uint64_t vertex_end = uint64_t(max_index) + uint32_t(std::max(state.base_vertex, 0)) + 1;
  auto* memory = rex::system::kernel_memory();
  for (uint32_t s = 0; s < 4; ++s) {
    if (!used[s]) continue;
    const uint32_t stride = uint32_t(REX_LOAD_U8(dev + 0x3268 + s)) * 4;
    const uint32_t address = REX_LOAD_U32(dev + 0x778 - s * 8) & ~3u;
    // The second word is the size with the endian mode in its low 2 bits.
    const uint32_t size_word = REX_LOAD_U32(dev + 0x778 - s * 8 + 4);
    const uint32_t available = size_word & ~3u;
    state.streams[s].endian = size_word & 3;
    if (!stride || !address) return;
    const uint64_t size = std::min<uint64_t>(vertex_end * stride, available) & ~uint64_t(3);
    if (!size || size > (16u << 20)) return;
    state.streams[s].stride = stride;
    state.streams[s].address = address;
    state.streams[s].guest = memory->TranslatePhysical(address);
    state.streams[s].guest_size = size_t(size);
  }
  NativeDraw(state);
}

REX_EXTERN(__imp__sub_82BA3CF8);
REX_HOOK_RAW(sub_82BA3CF8) {
  if (g_inline.active && g_inline.device == ctx.r3.u32) {
    const int32_t dump_frame = REXCVAR_GET(wwe2k15_native_dump_frame);
    if (dump_frame > 0 && NativeLastFrame() + 1 == uint64_t(dump_frame)) {
      DumpInlineDraw(base, g_inline, true);
      LogDrawTarget(base, g_inline.device);
    } else if (g_dumped_draws.fetch_add(1) < REXCVAR_GET(wwe2k15_native_dump_draws)) {
      DumpInlineDraw(base, g_inline, false);
    }
    if (NativeRendererEnabled()) {
      SubmitInlineDraw(base, g_inline);
    }
    g_inline.active = false;
  }
  __imp__sub_82BA3CF8(ctx, base);
}

// CreateVertexDeclaration (17.5): r3 = D3DVERTEXELEMENT9 array ending with
// stream 0xFF.
REX_EXTERN(__imp__sub_82BA36C0);
REX_HOOK_RAW(sub_82BA36C0) {
  const uint32_t elements = ctx.r3.u32;
  std::vector<uint8_t> copy;
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t entry = elements + i * 12;
    copy.insert(copy.end(), base + entry, base + entry + 12);
    if (REX_LOAD_U16(entry) == 0xFF) {
      break;
    }
  }
  __imp__sub_82BA36C0(ctx, base);
  if (const uint32_t object = ctx.r3.u32) {
    TrackedResource resource{ResourceKind::kVertexDeclaration, object};
    resource.elements = std::move(copy);
    Registry().Add(std::move(resource));
  }
}
