// Native renderer research hooks (docs/native-renderer.md).
//
// Phase 2: prove that a project definition replaces a generated (weak) guest
// function on this toolchain.
// Phase 3: trace what the game sends to D3D for a few frames. Every hook logs
// and then calls the original, so rendering is unchanged.
//
// Tracing is off unless --wwe2k15_trace_file is set. It then records frames
// [wwe2k15_trace_start_frame, wwe2k15_trace_start_frame + wwe2k15_trace_frames).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "generated/default/wwe2k15_pch.h"

REXCVAR_DEFINE_STRING(wwe2k15_trace_file, "", "Debug",
                      "Write a D3D call trace of a few frames to this file (empty: off)");
REXCVAR_DEFINE_INT32(wwe2k15_trace_start_frame, 600, "Debug",
                     "First presented frame to trace");
REXCVAR_DEFINE_INT32(wwe2k15_trace_frames, 2, "Debug", "Number of frames to trace");
REXCVAR_DEFINE_STRING(wwe2k15_shader_dump_dir, "", "Debug",
                      "Scan guest memory for Xbox 360 shader containers and save new ones here "
                      "(empty: off). The files are game data: keep them private.");
REXCVAR_DEFINE_INT32(wwe2k15_shader_dump_interval, 600, "Debug",
                     "Presented frames between shader-container scans");

namespace wwe2k15::native {
void LogResourceSummary();          // native/native_renderer.cpp
void NativeOnPresent(uint64_t frame);  // native/native_vulkan.cpp
void NativeOnResolve(uint8_t* base, uint32_t device, uint32_t flags, uint32_t rect,
                     uint32_t dest_texture, uint32_t clear_color, float clear_z,
                     uint32_t clear_stencil);  // native/native_renderer.cpp
void NativeOnClear(uint8_t* base, uint32_t device, uint32_t count, uint32_t rects,
                   uint32_t flags, uint32_t color, float z,
                   uint32_t stencil);  // native/native_renderer.cpp
void NativeOnIndexedDraw(uint8_t* base, uint32_t device, uint32_t primitive,
                         uint32_t base_vertex, uint32_t start_index,
                         uint32_t index_count);  // native/native_renderer.cpp
}

namespace {

// Phase 4: Xbox 360 compiled shader containers (the format XenosRecomp reads)
// begin with big-endian flags 0x102A11xx, then virtualSize and physicalSize;
// the container is virtualSize + physicalSize bytes and fields 0x1C/0x20 are 0.
// The game keeps them compressed in .pac archives, so they are collected from
// guest memory after loading (docs/native-renderer.md, Phase 4).
std::unordered_set<uint64_t> g_dumped_shaders;

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h;
}

// Scans one readable host range; returns the number of containers found. SEH
// guards against pages whose host protection changed under the scan.
uint32_t ScanRangeForShaders(const uint8_t* host, uint32_t size, uint32_t guest_base,
                             const std::filesystem::path& dir, uint32_t* out_new) {
  uint32_t found = 0;
  __try {
    for (uint32_t i = 0; i + 0x24 <= size; i += 4) {
      const uint8_t* p = host + i;
      if (p[0] != 0x10 || p[1] != 0x2A || p[2] != 0x11) {
        continue;
      }
      const uint32_t total = LoadBe32(p + 4) + LoadBe32(p + 8);
      if (total < 0x24 || total > size - i || total > 0x100000 || LoadBe32(p + 0x1C) != 0 ||
          LoadBe32(p + 0x20) != 0) {
        continue;
      }
      ++found;
      const uint64_t hash = Fnv1a64(p, total);
      if (g_dumped_shaders.insert(hash).second) {
        const auto file = dir / fmt::format("{:016X}.bin", hash);
        if (std::FILE* f = std::fopen(file.string().c_str(), "wb")) {
          std::fwrite(p, 1, total, f);
          std::fclose(f);
          ++*out_new;
        }
      }
    }
  } __except (1) {
    REXGPU_WARN("[native-renderer] shader scan: fault in guest range {:08X}+{:X}", guest_base,
                size);
  }
  return found;
}

void ScanGuestMemoryForShaders(const std::string& dir_name) {
  auto* memory = rex::system::kernel_memory();
  const std::filesystem::path dir(dir_name);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  uint32_t found = 0, added = 0;
  auto walk = [&](rex::memory::BaseHeap* heap, bool physical) {
    if (!heap) {
      return;
    }
    const uint32_t begin = heap->heap_base();
    const uint64_t end = uint64_t(begin) + heap->heap_size();
    uint64_t address = begin;
    while (address < end) {
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(uint32_t(address), &info) || !info.region_size) {
        address += 0x1000;
        continue;
      }
      if ((info.state & rex::memory::kMemoryAllocationCommit) &&
          (info.protect & rex::memory::kMemoryProtectRead)) {
        const uint8_t* host = physical ? memory->TranslatePhysical(info.base_address)
                                       : memory->TranslateVirtual(info.base_address);
        found += ScanRangeForShaders(host, info.region_size, info.base_address, dir, &added);
      }
      address = uint64_t(info.base_address) + info.region_size;
    }
  };
  // Physical memory (0-512 MB) through the unwatched physical view, plus the
  // regular virtual heaps where loaded files usually live.
  walk(memory->GetPhysicalHeap(), true);
  for (uint32_t base : {0x40000000u, 0x80000000u, 0x90000000u}) {
    walk(memory->LookupHeap(base), false);
  }
  REXGPU_INFO("[native-renderer] shader scan: {} containers in memory, {} new, {} saved in total",
              found, added, g_dumped_shaders.size());
}

std::atomic<bool> g_tracing{false};
std::atomic<uint64_t> g_frame{0};
std::mutex g_trace_mutex;
std::FILE* g_trace = nullptr;

// Per traced frame, for the summary line in the log.
uint32_t g_commands = 0;
uint32_t g_draws = 0;
uint32_t g_render_states = 0;
uint32_t g_sampler_states = 0;

// D3D functions that build PM4 WAIT_REG_MEM packets (section 4.2), counted to
// see what the GPU command processor waits for during slow stretches
// (docs/native-renderer.md, section 15).
constexpr uint32_t kWaitBuilders[] = {0x82B9F3D0, 0x82BA0270, 0x82BA03E0, 0x82BA1218,
                                      0x82BA5AE8, 0x82BA5D80, 0x82BA89A8, 0x82BA8CF8,
                                      0x82BAB6C8, 0x82BAFCD0};
std::atomic<uint32_t> g_wait_builder_calls[std::size(kWaitBuilders)];
std::atomic<uint32_t> g_wait_builder_last_lr[std::size(kWaitBuilders)];

// B1 (docs/native-renderer.md, 17.1): every other D3D function the game calls,
// counted with the arguments and caller of the latest call.
struct ApiCounter {
  uint32_t address;
  std::atomic<uint32_t> calls;
  std::atomic<uint32_t> r3, r4, r5, r6, lr;
};
#define API_COUNT(index, addr) {0x##addr},
ApiCounter g_api[] = {
#include "d3d_api_trace.inc"
};
#undef API_COUNT

void LogApiCounts() {
  for (auto& api : g_api) {
    if (const uint32_t n = api.calls.exchange(0, std::memory_order_relaxed)) {
      REXGPU_INFO("[native-renderer] api {:08X} x{} last(r3={:08X} r4={:08X} r5={:08X} "
                  "r6={:08X} lr={:08X})",
                  api.address, n, api.r3.load(), api.r4.load(), api.r5.load(), api.r6.load(),
                  api.lr.load());
    }
  }
}

void LogWaitBuilderCounts() {
  std::string line;
  for (size_t i = 0; i < std::size(kWaitBuilders); ++i) {
    const uint32_t n = g_wait_builder_calls[i].exchange(0, std::memory_order_relaxed);
    if (n) {
      line += fmt::format(" {:08X}x{}(lr {:08X})", kWaitBuilders[i], n,
                          g_wait_builder_last_lr[i].load(std::memory_order_relaxed));
    }
  }
  REXGPU_INFO("[native-renderer] WAIT_REG_MEM builders in the last 600 frames:{}",
              line.empty() ? " none" : line);
}

template <typename... Args>
void Trace(fmt::format_string<Args...> format, Args&&... args) {
  std::lock_guard lock(g_trace_mutex);
  if (g_trace) {
    fmt::print(g_trace, format, std::forward<Args>(args)...);
    std::fputc('\n', g_trace);
  }
}

void OnPresent() {
  const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  if (frame == 1 || frame % 600 == 0) {
    REXGPU_INFO("[native-renderer] present hook: frame {}", frame);
    LogWaitBuilderCounts();
    LogApiCounts();
    wwe2k15::native::LogResourceSummary();
  }
  wwe2k15::native::NativeOnPresent(frame);
  const std::string& dump_dir = REXCVAR_GET(wwe2k15_shader_dump_dir);
  const int32_t dump_interval = REXCVAR_GET(wwe2k15_shader_dump_interval);
  if (!dump_dir.empty() && dump_interval > 0 && frame % uint32_t(dump_interval) == 0) {
    ScanGuestMemoryForShaders(dump_dir);
  }
  const std::string& path = REXCVAR_GET(wwe2k15_trace_file);
  if (path.empty()) {
    return;
  }
  const uint64_t start = static_cast<uint32_t>(REXCVAR_GET(wwe2k15_trace_start_frame));
  const uint64_t end = start + static_cast<uint32_t>(REXCVAR_GET(wwe2k15_trace_frames));
  std::lock_guard lock(g_trace_mutex);
  if (g_trace) {
    REXGPU_INFO("[native-renderer] trace frame {}: {} commands, {} draws, {} render states, "
                "{} sampler states",
                frame - 1, g_commands, g_draws, g_render_states, g_sampler_states);
  }
  g_commands = g_draws = g_render_states = g_sampler_states = 0;
  if (frame == start) {
    g_trace = std::fopen(path.c_str(), "w");
    if (!g_trace) {
      REXGPU_ERROR("[native-renderer] cannot open trace file {}", path);
      return;
    }
    REXGPU_INFO("[native-renderer] tracing frames {}..{} to {}", start, end - 1, path);
    g_tracing.store(true, std::memory_order_relaxed);
  } else if (frame == end && g_trace) {
    g_tracing.store(false, std::memory_order_relaxed);
    std::fclose(g_trace);
    g_trace = nullptr;
    REXGPU_INFO("[native-renderer] trace finished");
    return;
  }
  if (g_trace) {
    fmt::print(g_trace, "FRAME {}\n", frame);
  }
}

bool Tracing() { return g_tracing.load(std::memory_order_relaxed); }

}  // namespace

// 0x82BA5FA8: D3D swap/present internals (the only caller of VdSwap).
REX_EXTERN(__imp__sub_82BA5FA8);
REX_HOOK_RAW(sub_82BA5FA8) {
  OnPresent();
  __imp__sub_82BA5FA8(ctx, base);
}

// 0x82BA4F40: CreateDevice (outer); logged on every run.
REX_EXTERN(__imp__sub_82BA4F40);
REX_HOOK_RAW(sub_82BA4F40) {
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32,
                 r7 = ctx.r7.u32, r8 = ctx.r8.u32;
  __imp__sub_82BA4F40(ctx, base);
  REXGPU_INFO("[native-renderer] CreateDevice(r3={:08X} r4={:08X} r5={:08X} r6={:08X} "
              "r7={:08X} r8={:08X}) -> {:08X}",
              r3, r4, r5, r6, r7, r8, ctx.r3.u32);
}

// 0x82AA7EB0: the game's render-command player; command type = **r4.
REX_EXTERN(__imp__sub_82AA7EB0);
REX_HOOK_RAW(sub_82AA7EB0) {
  if (Tracing()) {
    const uint32_t record = REX_LOAD_U32(ctx.r4.u32);
    const uint32_t type = REX_LOAD_U32(record);
    Trace("CMD {} record={:08X}", type, record);
    ++g_commands;
  }
  __imp__sub_82AA7EB0(ctx, base);
}

// Physical-memory allocation wrappers around MmAllocatePhysicalMemoryEx
// (XPhysicalAlloc-style). Logs failed requests to find the source of the
// "PhysicalHeap::Alloc unable to alloc" bursts (docs/native-renderer.md, 11.9).
#define TRACE_PHYS_ALLOC(addr)                                                              \
  REX_EXTERN(__imp__sub_##addr);                                                            \
  REX_HOOK_RAW(sub_##addr) {                                                                \
    static std::atomic<uint32_t> failures{0};                                               \
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32,      \
                   r7 = ctx.r7.u32, lr = static_cast<uint32_t>(ctx.lr);                     \
    __imp__sub_##addr(ctx, base);                                                           \
    if (ctx.r3.u32 == 0) {                                                                  \
      const uint32_t n = failures.fetch_add(1, std::memory_order_relaxed) + 1;              \
      if (n <= 20 || n % 250 == 0) {                                                        \
        REXSYS_WARN("[native-renderer] " #addr " failed #{}: r3={:08X} r4={:08X} r5={:08X} " \
                    "r6={:08X} r7={:08X} lr={:08X}",                                        \
                    n, r3, r4, r5, r6, r7, lr);                                             \
      }                                                                                     \
    }                                                                                       \
  }

TRACE_PHYS_ALLOC(82261418)
TRACE_PHYS_ALLOC(822614A0)
// Their callers, to walk up to the code that asks for too much.
TRACE_PHYS_ALLOC(82261530)
TRACE_PHYS_ALLOC(8225F848)

#define COUNT_WAIT_BUILDER(index, addr)                                                    \
  REX_EXTERN(__imp__sub_##addr);                                                          \
  REX_HOOK_RAW(sub_##addr) {                                                              \
    g_wait_builder_calls[index].fetch_add(1, std::memory_order_relaxed);                  \
    g_wait_builder_last_lr[index].store(uint32_t(ctx.lr), std::memory_order_relaxed);     \
    __imp__sub_##addr(ctx, base);                                                         \
  }

COUNT_WAIT_BUILDER(0, 82B9F3D0)
COUNT_WAIT_BUILDER(1, 82BA0270)
COUNT_WAIT_BUILDER(2, 82BA03E0)
COUNT_WAIT_BUILDER(3, 82BA1218)
COUNT_WAIT_BUILDER(4, 82BA5AE8)
COUNT_WAIT_BUILDER(5, 82BA5D80)
COUNT_WAIT_BUILDER(6, 82BA89A8)
COUNT_WAIT_BUILDER(7, 82BA8CF8)
COUNT_WAIT_BUILDER(8, 82BAB6C8)
COUNT_WAIT_BUILDER(9, 82BAFCD0)

#define API_COUNT(index, addr)                                                  \
  REX_EXTERN(__imp__sub_##addr);                                                \
  REX_HOOK_RAW(sub_##addr) {                                                    \
    auto& api = g_api[index];                                                   \
    api.calls.fetch_add(1, std::memory_order_relaxed);                          \
    api.r3.store(ctx.r3.u32, std::memory_order_relaxed);                        \
    api.r4.store(ctx.r4.u32, std::memory_order_relaxed);                        \
    api.r5.store(ctx.r5.u32, std::memory_order_relaxed);                        \
    api.r6.store(ctx.r6.u32, std::memory_order_relaxed);                        \
    api.lr.store(uint32_t(ctx.lr), std::memory_order_relaxed);                  \
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;           \
    const uint32_t r6 = ctx.r6.u32, r7 = ctx.r7.u32, r8 = ctx.r8.u32;           \
    const uint32_t r10 = ctx.r10.u32;                                           \
    const float f1 = float(ctx.f1.f64);                                         \
    __imp__sub_##addr(ctx, base);                                               \
    if constexpr (0x##addr == 0x82BA2370) { /* Resolve */                       \
      /* ClearStencil (on the stack) is taken as 0. */                          \
      wwe2k15::native::NativeOnResolve(base, r3, r4, r5, r6, r10, f1, 0);       \
    } else if constexpr (0x##addr == 0x82BA7AA0) { /* Clear */                  \
      wwe2k15::native::NativeOnClear(base, r3, r4, r5, r6, r7, f1, r8);         \
    }                                                                           \
  }
#include "d3d_api_trace.inc"
#undef API_COUNT

// Draw-packet builders (they write PM4 DRAW_INDX / DRAW_INDX_2).
#define TRACE_DRAW(addr, label)                                                         \
  REX_EXTERN(__imp__sub_##addr);                                                        \
  REX_HOOK_RAW(sub_##addr) {                                                            \
    if (Tracing()) {                                                                    \
      Trace(label " r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X}", ctx.r4.u32,      \
            ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);                            \
      ++g_draws;                                                                        \
    }                                                                                   \
    if constexpr (0x##addr == 0x82BA41C8) { /* indexed draw */                          \
      wwe2k15::native::NativeOnIndexedDraw(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,    \
                                           ctx.r6.u32, ctx.r7.u32);                     \
    }                                                                                   \
    __imp__sub_##addr(ctx, base);                                                       \
  }

TRACE_DRAW(82BA41C8, "DRAW 82BA41C8")
// 82BA3D08 / 82BA3CF8 (begin/end inline vertices) are hooked by
// native/native_renderer.cpp.
TRACE_DRAW(82BA4DC8, "DRAW_INIT 82BA4DC8")
TRACE_DRAW(82B9F708, "DRAW2 82B9F708")
TRACE_DRAW(82B9F860, "DRAW2 82B9F860")
TRACE_DRAW(82BA38A0, "DRAW2 82BA38A0")
TRACE_DRAW(82BA6828, "DRAW2 82BA6828")
TRACE_DRAW(82BB3FD8, "DRAW2 82BB3FD8")
TRACE_DRAW(82BBB1D8, "DRAW2 82BBB1D8")


// Render-state setters from the table at 0x835BD9C0 (101 x {getter, setter,
// default}); the D3DRS value is index * 4. Entries 0-9 point to a blr stub.
// Setter arguments: r3 = device, r4 = value.
#define TRACE_RS(state, addr)                                         \
  REX_EXTERN(__imp__sub_##addr);                                      \
  REX_HOOK_RAW(sub_##addr) {                                          \
    if (Tracing()) {                                                  \
      Trace("RS " #state " = {:08X}", ctx.r4.u32);                    \
      ++g_render_states;                                              \
    }                                                                 \
    __imp__sub_##addr(ctx, base);                                     \
  }

TRACE_RS(0x028, 82B9BEC8) TRACE_RS(0x02C, 82B9BF38) TRACE_RS(0x030, 82B9BF08)
TRACE_RS(0x034, 82B9B7D0) TRACE_RS(0x038, 82B9B7A0) TRACE_RS(0x03C, 82B9B838)
TRACE_RS(0x040, 82B9BBC8) TRACE_RS(0x044, 82B9BCF0) TRACE_RS(0x048, 82B9B958)
TRACE_RS(0x04C, 82B9B9E8) TRACE_RS(0x050, 82B9B8C8) TRACE_RS(0x054, 82B9BAE8)
TRACE_RS(0x058, 82B9BB58) TRACE_RS(0x05C, 82B9BA78) TRACE_RS(0x060, 82B9B800)
TRACE_RS(0x064, 82B9BC60) TRACE_RS(0x068, 82B9BCC0) TRACE_RS(0x06C, 82B9BF70)
TRACE_RS(0x070, 82B9BFB0) TRACE_RS(0x074, 82B9C018) TRACE_RS(0x078, 82B9C050)
TRACE_RS(0x07C, 82B9C088) TRACE_RS(0x080, 82B9BFE8) TRACE_RS(0x084, 82B9C188)
TRACE_RS(0x088, 82B9C1A8) TRACE_RS(0x08C, 82B9C1C8) TRACE_RS(0x090, 82B9C0E8)
TRACE_RS(0x094, 82B9C120) TRACE_RS(0x098, 82B9C158) TRACE_RS(0x09C, 82B9C0B8)
TRACE_RS(0x0A0, 82B9C1E8) TRACE_RS(0x0A4, 82B9C208) TRACE_RS(0x0A8, 82B9C228)
TRACE_RS(0x0AC, 82B9C248) TRACE_RS(0x0B0, 82B9C578) TRACE_RS(0x0B4, 82B9C5D0)
TRACE_RS(0x0B8, 82B9C568) TRACE_RS(0x0BC, 82B9C620) TRACE_RS(0x0C0, 82B9C418)
TRACE_RS(0x0C4, 82B9C448) TRACE_RS(0x0C8, 82B9E3A0) TRACE_RS(0x0CC, 82B9C2A8)
TRACE_RS(0x0D0, 82B9C370) TRACE_RS(0x0D4, 82B9C468) TRACE_RS(0x0D8, 82B9C4A8)
TRACE_RS(0x0DC, 82B9C4E8) TRACE_RS(0x0E0, 82B9C528) TRACE_RS(0x0E4, 82B9CCF0)
TRACE_RS(0x0E8, 82B9CC80) TRACE_RS(0x0EC, 82B9CCB8) TRACE_RS(0x0F0, 82B9C670)
TRACE_RS(0x0F4, 82B9C690) TRACE_RS(0x0F8, 82B9C6B8) TRACE_RS(0x0FC, 82B9C6E0)
TRACE_RS(0x100, 82B9C708) TRACE_RS(0x104, 82B9C730) TRACE_RS(0x108, 82B9C758)
TRACE_RS(0x10C, 82B9C780) TRACE_RS(0x110, 82B9C7A0) TRACE_RS(0x114, 82B9C7C0)
TRACE_RS(0x118, 82B9C7E8) TRACE_RS(0x11C, 82B9C810) TRACE_RS(0x120, 82B9C838)
TRACE_RS(0x124, 82B9C860) TRACE_RS(0x128, 82B9C888) TRACE_RS(0x12C, 82B9C8B0)
TRACE_RS(0x130, 82B9C9D0) TRACE_RS(0x134, 82B9CA20) TRACE_RS(0x138, 82B9CAB8)
TRACE_RS(0x13C, 82B9CB50) TRACE_RS(0x140, 82B9CBE8) TRACE_RS(0x144, 82B9CD28)
TRACE_RS(0x148, 82B9CD60) TRACE_RS(0x14C, 82B9CD90) TRACE_RS(0x150, 82B9CDB8)
TRACE_RS(0x154, 82B9CDE8) TRACE_RS(0x158, 82B9CE10) TRACE_RS(0x15C, 82B9CE40)
TRACE_RS(0x160, 82B9CE70) TRACE_RS(0x164, 82B9CE98) TRACE_RS(0x168, 82B9CF60)
TRACE_RS(0x16C, 82B9CF90) TRACE_RS(0x170, 82B9CFC0) TRACE_RS(0x174, 82B9CFF0)
TRACE_RS(0x178, 82B9D010) TRACE_RS(0x17C, 82B9D020) TRACE_RS(0x180, 82B9CEC8)
TRACE_RS(0x184, 82B9CEF8) TRACE_RS(0x188, 82B9BDF0) TRACE_RS(0x18C, 82B9BE28)
TRACE_RS(0x190, 82B9BE98)

// Sampler-state setters from the table at 0x835BDE80 (20 x {getter, setter,
// default}). Setter arguments: r3 = device, r4 = sampler, r5 = value.
#define TRACE_SS(type, addr)                                                     \
  REX_EXTERN(__imp__sub_##addr);                                                 \
  REX_HOOK_RAW(sub_##addr) {                                                     \
    if (Tracing()) {                                                             \
      Trace("SS type=" #type " sampler={} = {:08X}", ctx.r4.u32, ctx.r5.u32);     \
      ++g_sampler_states;                                                        \
    }                                                                            \
    __imp__sub_##addr(ctx, base);                                                \
  }

TRACE_SS(0, 82B9D7B8) TRACE_SS(1, 82B9D808) TRACE_SS(2, 82B9D858) TRACE_SS(3, 82B9D748)
TRACE_SS(4, 82B9D1E8) TRACE_SS(5, 82B9D040) TRACE_SS(6, 82B9D388) TRACE_SS(7, 82B9D5A8)
TRACE_SS(8, 82B9D648) TRACE_SS(9, 82B9D488) TRACE_SS(10, 82B9D2E8) TRACE_SS(11, 82B9D140)
TRACE_SS(12, 82B9D3E0) TRACE_SS(13, 82B9D6C8) TRACE_SS(14, 82B9D8A8) TRACE_SS(15, 82B9D500)
TRACE_SS(16, 82B9D900) TRACE_SS(17, 82B9D958) TRACE_SS(18, 82B9D9B0) TRACE_SS(19, 82B9DA08)
