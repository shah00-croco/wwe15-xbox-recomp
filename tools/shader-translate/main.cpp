// Offline Xenos shader translation with the ReXGlue SDK SPIR-V translator
// (docs/native-renderer.md, Phase 5 / M1).
//
// Input: a folder of microcode files as written by the SDK's --dump_shaders
// (shader_<hash>.ucode.bin.vert / .frag, dwords in host byte order).
// Output: <hash>.vert.spv / <hash>.frag.spv for every shader that translates,
// configured for the plain render-target path (no fragment shader interlock),
// and a results.csv. The outputs are derived from game data: keep them private.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

namespace fs = std::filesystem;
using rex::graphics::Shader;
using rex::graphics::SpirvShaderTranslator;
namespace xenos = rex::graphics::xenos;

namespace {

// Analyze and translate one shader. Returns 1 for a valid translation, 0 for an
// invalid one and -1 when the translator crashed (SEH keeps the batch going).
int TranslateOne(SpirvShaderTranslator& translator, Shader& shader, bool vertex,
                 rex::string::StringBuffer& disasm, std::vector<uint8_t>& spirv_out) {
  __try {
    disasm.Reset();
    shader.AnalyzeUcode(disasm);
    // No register state offline: allow the maximum dynamically addressable
    // register count, the conservative choice.
    const uint32_t registers = xenos::kMaxShaderTempRegisters;
    const uint64_t modification =
        vertex ? translator.GetDefaultVertexShaderModification(registers)
               : translator.GetDefaultPixelShaderModification(registers);
    Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
    if (!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid()) {
      return 0;
    }
    spirv_out = translation->translated_binary();
    return 1;
  } __except (1) {
    return -1;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "Usage: wwe2k15-shader-translate <ucode dir> <output dir>\n");
    return 2;
  }
  const fs::path input(argv[1]), output(argv[2]);
  fs::create_directories(output);

  SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true),
                                   /*native_2x_msaa_with_attachments=*/false,
                                   /*native_2x_msaa_no_attachments=*/false,
                                   /*edram_fragment_shader_interlock=*/false);
  rex::string::StringBuffer disasm;

  std::ofstream csv(output / "results.csv");
  csv << "file,type,dwords,valid,spirv_bytes\n";
  uint32_t total = 0, ok = 0, crashed = 0;
  for (const auto& entry : fs::directory_iterator(input)) {
    const std::string name = entry.path().filename().string();
    const bool vertex = name.ends_with(".ucode.bin.vert");
    if (!vertex && !name.ends_with(".ucode.bin.frag")) {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    std::vector<uint32_t> dwords(fs::file_size(entry.path()) / sizeof(uint32_t));
    in.read(reinterpret_cast<char*>(dwords.data()), dwords.size() * sizeof(uint32_t));
    // shader_<16 hex digits>.ucode...
    const uint64_t hash = std::stoull(name.substr(7, 16), nullptr, 16);

    Shader shader(vertex ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel, hash,
                  dwords.data(), dwords.size(), std::endian::native);
    std::vector<uint8_t> spirv;
    const int result = TranslateOne(translator, shader, vertex, disasm, spirv);
    const bool valid = result == 1;
    ++total;
    size_t bytes = 0;
    if (result < 0) {
      ++crashed;
    }
    if (valid) {
      ++ok;
      bytes = spirv.size();
      std::ofstream out(output / (name.substr(7, 16) + (vertex ? ".vert.spv" : ".frag.spv")),
                        std::ios::binary);
      out.write(reinterpret_cast<const char*>(spirv.data()), spirv.size());
    }
    csv << name << ',' << (vertex ? "vs" : "ps") << ',' << dwords.size() << ','
        << (result < 0 ? "crash" : valid ? "1" : "0") << ',' << bytes << '\n';
    csv.flush();
  }
  std::printf("translated %u of %u shaders (%u crashed the translator)\n", ok, total, crashed);
  return ok == total ? 0 : 1;
}
