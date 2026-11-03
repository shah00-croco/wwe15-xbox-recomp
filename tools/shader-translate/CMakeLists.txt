# Offline Xenos -> SPIR-V shader translation with the ReXGlue SDK translator
# (docs/native-renderer.md, Phase 5 / M1). Standalone: the game build does not
# use it.
#
# The SDK's GPU plugin does not export its translator, so its BSD-3 sources are
# compiled here straight from an SDK source checkout:
#   cmake -S tools/shader-translate -B out/build/shader-translate -G Ninja
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<sdk install prefix>
#         -DREXGLUE_SDK_SOURCE_DIR=<sdk source checkout>

cmake_minimum_required(VERSION 3.25)
project(wwe2k15_shader_translate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXGLUE_SDK_SOURCE_DIR "" CACHE PATH "ReXGlue SDK source checkout (same version as the install)")
if(NOT EXISTS "${REXGLUE_SDK_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator.cpp")
    message(FATAL_ERROR "Set REXGLUE_SDK_SOURCE_DIR to the ReXGlue SDK source checkout.")
endif()

find_package(rexglue REQUIRED)

set(SDK_GRAPHICS "${REXGLUE_SDK_SOURCE_DIR}/src/graphics")
add_executable(wwe2k15-shader-translate
    main.cpp
    ${SDK_GRAPHICS}/flags.cpp
    ${SDK_GRAPHICS}/registers.cpp
    ${SDK_GRAPHICS}/util/draw.cpp
    ${SDK_GRAPHICS}/format/ucode.cpp
    ${SDK_GRAPHICS}/pipeline/shader/shader.cpp
    ${SDK_GRAPHICS}/pipeline/shader/interpreter.cpp
    ${SDK_GRAPHICS}/pipeline/shader/translator.cpp
    ${SDK_GRAPHICS}/pipeline/shader/translator_disasm.cpp
    ${SDK_GRAPHICS}/pipeline/shader/spirv.cpp
    ${SDK_GRAPHICS}/pipeline/shader/spirv_builder.cpp
    ${SDK_GRAPHICS}/pipeline/shader/spirv_translator.cpp
    ${SDK_GRAPHICS}/pipeline/shader/spirv_translator_alu.cpp
    ${SDK_GRAPHICS}/pipeline/shader/spirv_translator_fetch.cpp
    ${SDK_GRAPHICS}/pipeline/shader/spirv_translator_memexport.cpp
    ${SDK_GRAPHICS}/pipeline/shader/spirv_translator_rb.cpp
)
target_include_directories(wwe2k15-shader-translate PRIVATE
    "${REXGLUE_SDK_SOURCE_DIR}/thirdparty/glslang")
target_link_libraries(wwe2k15-shader-translate PRIVATE
    rex::runtime rex::renderdoc rex::SPIRV rex::glslang rex::MachineIndependent rex::GenericCodeGen
    rex::OSDependent rex::OGLCompiler rex::xxhash)
