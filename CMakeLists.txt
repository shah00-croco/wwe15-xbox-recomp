# wwe2k15 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(wwe2k15 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The game is not part of this repository; codegen translates the user's copy.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xex")
    message(FATAL_ERROR
        "assets/default.xex not found. Extract your own copy of the game into "
        "assets/ (see README.md), then run tools/check-game.py.")
endif()

include(generated/rexglue.cmake)

# Sources
set(WWE2K15_SOURCES
    src/main.cpp
    src/renderer_hooks.cpp
    src/native/native_renderer.cpp
    src/native/native_vulkan.cpp
)

if(WIN32)
    add_executable(wwe2k15 WIN32 ${WWE2K15_SOURCES})
else()
    add_executable(wwe2k15 ${WWE2K15_SOURCES})
endif()

# Native renderer GPU plugin (docs/native-renderer.md, Phase 5). It compiles the
# SDK's backend-independent GPU sources, which the installed SDK does not export,
# from an SDK source checkout of the same version. Optional: without
# REXGLUE_SDK_SOURCE_DIR only the xenos plugin is built.
set(REXGLUE_SDK_SOURCE_DIR "$ENV{REXGLUE_SDK_SOURCE_DIR}" CACHE PATH
    "ReXGlue SDK source checkout, for the native renderer plugin")
set(WWE2K15_GPU_PLUGINS xenos)
if(REXGLUE_SDK_SOURCE_DIR AND
   EXISTS "${REXGLUE_SDK_SOURCE_DIR}/src/graphics/command_processor.cpp")
    set(SDK_GRAPHICS "${REXGLUE_SDK_SOURCE_DIR}/src/graphics")
    add_library(rexgpu-native SHARED
        src/native/native_graphics_system.cpp
        ${SDK_GRAPHICS}/flags.cpp
        ${SDK_GRAPHICS}/register_file.cpp
        ${SDK_GRAPHICS}/registers.cpp
        ${SDK_GRAPHICS}/sampler_info.cpp
        ${SDK_GRAPHICS}/graphics_system.cpp
        ${SDK_GRAPHICS}/command_processor.cpp
        ${SDK_GRAPHICS}/util/draw.cpp
        ${SDK_GRAPHICS}/format/ucode.cpp
        ${SDK_GRAPHICS}/pipeline/shader/shader.cpp
        ${SDK_GRAPHICS}/pipeline/shader/translator.cpp
        ${SDK_GRAPHICS}/pipeline/shader/translator_disasm.cpp
    )
    target_link_libraries(rexgpu-native PRIVATE rex::runtime rex::renderdoc rex::xxhash)
    list(APPEND WWE2K15_GPU_PLUGINS native)

    # Profiling only: the SDK's Vulkan GPU emulation (the same sources as
    # rexgpu-xenos, without D3D12) with debug symbols in a PDB
    # (docs/native-renderer.md, section 15). Off by default.
    option(WWE2K15_PROFILING_PLUGIN "Build rexgpu-xenosprof with debug symbols" OFF)
    if(WWE2K15_PROFILING_PLUGIN OR "$ENV{WWE2K15_PROFILING_PLUGIN}")
        set(SDK_GRAPHICS_SOURCES
            flags.cpp register_file.cpp registers.cpp sampler_info.cpp
            graphics_system.cpp command_processor.cpp util/draw_extent_estimator.cpp
            util/draw.cpp packet_disassembler.cpp primitive_processor.cpp
            pipeline/render_target/cache.cpp shared_memory.cpp pipeline/texture/cache.cpp
            format/ucode.cpp pipeline/shader/shader.cpp pipeline/shader/interpreter.cpp
            pipeline/shader/translator.cpp pipeline/shader/translator_disasm.cpp
            pipeline/shader/spirv_builder.cpp pipeline/shader/spirv.cpp
            pipeline/shader/spirv_translator.cpp pipeline/shader/spirv_translator_alu.cpp
            pipeline/shader/spirv_translator_fetch.cpp
            pipeline/shader/spirv_translator_memexport.cpp
            pipeline/shader/spirv_translator_rb.cpp
            vulkan/graphics_system.cpp vulkan/deferred_command_buffer.cpp
            vulkan/command_processor.cpp vulkan/pipeline_cache.cpp
            vulkan/primitive_processor.cpp vulkan/render_target_cache.cpp vulkan/shader.cpp
            vulkan/shared_memory.cpp vulkan/texture_cache.cpp)
        list(TRANSFORM SDK_GRAPHICS_SOURCES PREPEND "${SDK_GRAPHICS}/")
        add_library(rexgpu-xenosprof SHARED
            src/native/xenos_profiling_plugin.cpp ${SDK_GRAPHICS_SOURCES})
        target_include_directories(rexgpu-xenosprof PRIVATE
            "${REXGLUE_SDK_SOURCE_DIR}/thirdparty/glslang")
        # -msse4.1 as in the SDK's own build (cmake/rexglue_helpers.cmake).
        target_compile_options(rexgpu-xenosprof PRIVATE -msse4.1 -g -gcodeview
                               -Wno-unused-variable)
        target_link_options(rexgpu-xenosprof PRIVATE LINKER:/DEBUG)
        target_link_libraries(rexgpu-xenosprof PRIVATE
            rex::runtime rex::renderdoc rex::xxhash rex::SPIRV rex::glslang
            rex::MachineIndependent rex::GenericCodeGen rex::OSDependent rex::OGLCompiler)
        list(APPEND WWE2K15_GPU_PLUGINS xenosprof)
    endif()
endif()

rexglue_setup_target(wwe2k15 GPU_PLUGINS ${WWE2K15_GPU_PLUGINS})
# The native renderer core uses the SDK's Vulkan UI headers, which include
# renderdoc_app.h.
target_link_libraries(wwe2k15 PRIVATE rex::renderdoc)
