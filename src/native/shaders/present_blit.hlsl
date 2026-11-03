// Full-screen copy of the native renderer's back buffer into the presenter's
// guest output image (docs/native-renderer.md, section 19.4), through the
// game's 256-entry gamma ramp, as the display controller applies it at
// scan-out (section 20.7).
// Build (embedded as src/native/shaders/present_blit_spirv.h), with a DXC
// that has SPIR-V code generation (.tools/XenosRecomp/thirdparty/dxc-bin):
//   dxc -spirv -T vs_6_0 -E VSMain present_blit.hlsl
//   dxc -spirv -T ps_6_0 -E PSMain present_blit.hlsl

[[vk::binding(0, 0)]] Texture2D<float4> g_Source;
[[vk::binding(1, 0)]] SamplerState g_Sampler;
// DC_LUT_30_COLOR entries: blue in bits 0-9, green 10-19, red 20-29.
[[vk::binding(2, 0)]] ByteAddressBuffer g_GammaRamp;

struct VSOutput {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertex_id : SV_VertexID) {
  // One triangle that covers the whole target.
  VSOutput output;
  output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
  output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
  return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
  // The back buffer and the guest output have the same size: one texel per
  // pixel, like the emulator's table lookup.
  float3 color = g_Source.Load(int3(input.position.xy, 0)).rgb;
  uint3 index = uint3(saturate(color) * 255.0 + 0.5);
  return float4(float((g_GammaRamp.Load(index.r * 4) >> 20) & 0x3FF),
                float((g_GammaRamp.Load(index.g * 4) >> 10) & 0x3FF),
                float(g_GammaRamp.Load(index.b * 4) & 0x3FF), 1023.0) / 1023.0;
}
