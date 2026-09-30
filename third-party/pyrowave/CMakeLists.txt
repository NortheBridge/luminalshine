# Vendored PyroWave GPU wavelet codec (Vulkan compute/fragment shaders).
# Upstream: Hans-Kristian Arntzen, integrated from WiVRn's proto/pyrowave branch.
#
# Produces a STATIC library target `pyrowave` that both Sunshine and
# moonlight-qt can link. The codec is build-system agnostic: GLSL shaders are
# compiled to a C++ table by tools/generate_shaders.py (shared with the qmake
# build) rather than CompileGLSL.cmake.
#
# Consumer requirements (see README.md):
#   * C++20
#   * Vulkan headers + vulkan_raii
#   * The host MUST construct exactly one `vk_allocator` (VMA) singleton before
#     creating PyroWave::Encoder / PyroWave::Decoder.

cmake_minimum_required(VERSION 3.20)
project(pyrowave LANGUAGES CXX)

set(PYROWAVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(PYROWAVE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

# --- Tooling: Python (for the shader generator) and glslangValidator --------
find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(NOT DEFINED PYROWAVE_GLSLANG)
    find_program(PYROWAVE_GLSLANG NAMES glslangValidator glslang)
endif()
if(NOT PYROWAVE_GLSLANG)
    message(FATAL_ERROR
        "pyrowave: glslangValidator not found. Install the Vulkan SDK / "
        "glslang-tools, or set -DPYROWAVE_GLSLANG=/path/to/glslangValidator.")
endif()

# --- Compile shaders -> C++ table at build time -----------------------------
file(GLOB PYROWAVE_SHADER_SOURCES CONFIGURE_DEPENDS
    "${PYROWAVE_ROOT}/shaders/*.comp"
    "${PYROWAVE_ROOT}/shaders/*.glsl"
    "${PYROWAVE_ROOT}/shaders/*.inc")

set(PYROWAVE_SHADERS_CPP ${PYROWAVE_GENERATED_DIR}/pyrowave_shaders.cpp)
set(PYROWAVE_SHADERS_H   ${PYROWAVE_GENERATED_DIR}/pyrowave_shaders.h)

add_custom_command(
    OUTPUT ${PYROWAVE_SHADERS_CPP} ${PYROWAVE_SHADERS_H}
    COMMAND ${Python3_EXECUTABLE} ${PYROWAVE_ROOT}/tools/generate_shaders.py
            --shader-dir ${PYROWAVE_ROOT}/shaders
            --output-dir ${PYROWAVE_GENERATED_DIR}
            --glslang ${PYROWAVE_GLSLANG}
            --target-env vulkan1.1
            --namespace pyrowave
    DEPENDS ${PYROWAVE_SHADER_SOURCES} ${PYROWAVE_ROOT}/tools/generate_shaders.py
    COMMENT "pyrowave: compiling GLSL shaders to SPIR-V C++ tables"
    VERBATIM)

add_custom_target(pyrowave_shaders DEPENDS ${PYROWAVE_SHADERS_CPP})

# --- Library ----------------------------------------------------------------
add_library(pyrowave STATIC
    src/pyrowave/pyrowave_common.cpp
    src/pyrowave/pyrowave_encoder.cpp
    src/pyrowave/pyrowave_decoder.cpp
    src/vk/allocation.cpp
    src/vk/vk_allocator.cpp
    src/vk/error_category.cpp
    src/vk/vk_mem_alloc.cpp
    ${PYROWAVE_SHADERS_CPP})

add_dependencies(pyrowave pyrowave_shaders)

target_compile_features(pyrowave PUBLIC cxx_std_20)

# This codec is extremely template-heavy to compile (Vulkan-Hpp RAII; the encoder/
# decoder TUs are 60-80 KB each). At -O3 + LTO the compiler's peak memory balloons,
# which on lower-RAM machines (or fragile/dev GCC) shows up as GC internal compiler
# errors ("internal compiler error: Segmentation fault" in gt_ggc_mx*) - and not
# necessarily in these TUs, but in whatever parallel job loses the memory race.
# The codec's runtime performance lives entirely in the SPIR-V shaders, so this
# driver C++ gains nothing measurable from -O3/LTO. Build it lean to keep the whole
# Sunshine build's footprint down. Scoped PRIVATE so it never touches consumers.
set_target_properties(pyrowave PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Appended after the global CXXFLAGS, so these win: LTO off, optimisation -O2.
    target_compile_options(pyrowave PRIVATE -fno-lto -O2)
endif()

# VULKAN_HPP_NO_STRUCT_CONSTRUCTORS is required by the codec sources AND by any
# consumer TU that includes the public headers (they use designated-init of
# vk:: structs), so it must be PUBLIC.
target_compile_definitions(pyrowave PUBLIC VULKAN_HPP_NO_STRUCT_CONSTRUCTORS)

target_include_directories(pyrowave PUBLIC
    ${PYROWAVE_ROOT}/src            # for "vk/...", "utils/...", "pyrowave/..."
    ${PYROWAVE_ROOT}/src/pyrowave   # for "pyrowave_common.h" etc.
    ${PYROWAVE_ROOT}/external)      # for <vk_mem_alloc.h>
target_include_directories(pyrowave PRIVATE
    ${PYROWAVE_GENERATED_DIR})      # for "pyrowave_shaders.h"

# Vulkan headers. We only need the HEADERS (the host app owns the device, dispatch
# loader and queue). CRITICAL: the codec and the host MUST use the SAME Vulkan
# headers / VK_HEADER_VERSION, otherwise vulkan_raii's DeviceDispatcher layout
# differs between translation units and raii handles (e.g. DescriptorPool) created
# in one TU crash when used in the other (wrong function-pointer offsets -> SIGSEGV
# in the ICD, or a getVkHeaderVersion assert in debug).
#
# When the host provides a header dir (PYROWAVE_VULKAN_HEADERS_DIR), use ONLY that
# and do NOT link Vulkan::Vulkan: that imported target would drag the SYSTEM headers
# into every consumer of this library (including the host's own src/pyrowave TUs),
# remixing two header versions. The host links the Vulkan loader itself.
if(DEFINED PYROWAVE_VULKAN_HEADERS_DIR)
    message(STATUS "pyrowave: using host-provided Vulkan headers: ${PYROWAVE_VULKAN_HEADERS_DIR}")
    target_include_directories(pyrowave PUBLIC ${PYROWAVE_VULKAN_HEADERS_DIR})
else()
    # Standalone / moonlight-qt build: fall back to discovered headers.
    find_package(VulkanHeaders QUIET)
    find_package(Vulkan QUIET)
    if(TARGET Vulkan::Headers)
        target_link_libraries(pyrowave PUBLIC Vulkan::Headers)
    elseif(DEFINED Vulkan_INCLUDE_DIRS)
        target_include_directories(pyrowave PUBLIC ${Vulkan_INCLUDE_DIRS})
    elseif(TARGET Vulkan::Vulkan)
        target_link_libraries(pyrowave PUBLIC Vulkan::Vulkan)
    endif()
endif()
