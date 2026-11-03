// Native renderer: what the game side hands to the Vulkan core for one draw
// (docs/native-renderer.md, section 20).

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace wwe2k15::native {

struct NativeDrawState {
  // Xenos primitive type (xenos::PrimitiveType) and vertex count (index count
  // for indexed draws).
  uint32_t primitive = 0;
  uint32_t vertex_count = 0;
  // Vertex streams 0-3 (D3D streams = binding numbers). Data exactly as in
  // guest memory (big-endian): in data for inline vertices, or guest bytes
  // read in place during NativeDraw (section 20.15). Empty if the stream is
  // not used.
  struct Stream {
    std::vector<uint8_t> data;
    const uint8_t* guest = nullptr;
    size_t guest_size = 0;
    uint32_t stride = 0;
    uint32_t address = 0;  // guest physical address (0 for inline vertices)
    uint32_t endian = 2;   // xenos::Endian of the fetch constant (8in32 by default)

    const uint8_t* bytes() const { return guest ? guest : data.data(); }
    size_t size() const { return guest ? guest_size : data.size(); }
  };
  std::array<Stream, 4> streams;
  // Indexed draws (82BA41C8): host-order indices; base_vertex is added to each
  // (VGT_INDX_OFFSET). Empty for inline-vertex draws.
  std::vector<uint32_t> indices;
  int32_t base_vertex = 0;
  // D3DVERTEXELEMENT9 entries (12 bytes each, big-endian, ending with stream 0xFF).
  std::vector<uint8_t> declaration;
  uint64_t declaration_key = 0;
  // Hashes of the compiled shader containers (names of the SPIR-V files).
  uint64_t vertex_shader = 0;
  uint64_t pixel_shader = 0;
  // Float constants from the device shadow, already in host byte order.
  std::array<uint32_t, 256 * 4> vertex_constants{};
  std::array<uint32_t, 256 * 4> pixel_constants{};
  // Boolean constants b0-b31 of the vertex and pixel shader (bit n = bn) and
  // loop constants 0-31 (count in bits 0-7, start 8-15, step 16-23), section
  // 20.10.
  uint32_t vertex_bools = 0, pixel_bools = 0;
  std::array<uint32_t, 32> loop_constants{};
  // Texture fetch constants 0-31 (6 words each, host byte order).
  std::array<std::array<uint32_t, 6>, 32> texture_fetch{};
  // Render state registers from the device's register shadow (section 20.4).
  uint32_t blend_control = 0;  // RB_BLENDCONTROL0
  uint32_t color_control = 0;  // RB_COLORCONTROL (alpha test)
  uint32_t depth_control = 0;  // RB_DEPTHCONTROL (depth and stencil tests)
  uint32_t stencil_ref_mask = 0;     // RB_STENCILREFMASK (front faces)
  uint32_t stencil_ref_mask_bf = 0;  // RB_STENCILREFMASK_BF (back faces)
  uint32_t color_mask = 0xF;   // RB_COLOR_MASK
  uint32_t alpha_ref = 0;      // RB_ALPHA_REF (float bits)
  uint32_t mode_control = 0;   // PA_SU_SC_MODE_CNTL (face culling)
  // Render target 0 (section 20.5): RB_SURFACE_INFO and RB_COLOR_INFO.
  uint32_t surface_info = 0;
  uint32_t color_info = 0;
  // PA_CL_VPORT_XSCALE, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET and
  // PA_CL_VTE_CNTL.
  std::array<float, 6> viewport{};
  uint32_t vte_control = 0;
};

using GuestRect = std::array<int32_t, 4>;  // D3DRECT: x1, y1, x2, y2

// Records one draw into the current frame (no-op unless the native renderer
// is enabled). Called on the guest render thread.
void NativeDraw(const NativeDrawState& state);

// Called after the game's Resolve (82BA2370) returns; arguments as passed to it
// (device, flags, source rect, destination texture, clear color pointer,
// clear depth, clear stencil).
void NativeOnResolve(uint8_t* base, uint32_t device, uint32_t flags, uint32_t rect,
                     uint32_t dest_texture, uint32_t clear_color, float clear_z,
                     uint32_t clear_stencil);
// Called before the game's indexed draw (82BA41C8): device, primitive type,
// base vertex, start index, index count.
void NativeOnIndexedDraw(uint8_t* base, uint32_t device, uint32_t primitive,
                         uint32_t base_vertex, uint32_t start_index, uint32_t index_count);
// Called after the game's Clear (82BA7AA0): device, rect count, rects, flags,
// color, depth, stencil.
void NativeOnClear(uint8_t* base, uint32_t device, uint32_t count, uint32_t rects,
                   uint32_t flags, uint32_t color, float z, uint32_t stencil);

// What a clear or a resolve's clear-after writes (section 20.10).
struct ClearValues {
  bool color = false, depth = false, stencil = false;
  std::array<float, 4> rgba{};  // red, green, blue, alpha
  float z = 1.0f;
  uint32_t stencil_value = 0;
};

// Core side: copies render target (surface_info, color_info) - or its depth
// buffer, if depth is set - into the texture whose fetch constant is
// dest_fetch, then clears that rect of the target with clear. No rect = the
// destination's size.
void NativeResolve(uint32_t surface_info, uint32_t color_info, bool depth,
                   const std::array<uint32_t, 6>& dest_fetch, bool has_rect, GuestRect rect,
                   const ClearValues& clear);
// Core side: clears render target (surface_info, color_info) and its depth
// buffer; no rects = the whole target.
void NativeClear(uint32_t surface_info, uint32_t color_info, const ClearValues& clear,
                 const std::vector<GuestRect>& rects);
// The number of the last frame presented (NativeOnPresent).
uint64_t NativeLastFrame();

}  // namespace wwe2k15::native
