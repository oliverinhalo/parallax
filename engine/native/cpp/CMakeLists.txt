# Parallax native reconstruction engine.
#
# Builds in two configurations:
#   * Host (Linux/macOS): static library + gtest suite + offline tools. Used by
#     CI and by developers to exercise the full pipeline on synthetic data.
#   * Android (NDK toolchain): shared library `libparallax_engine.so` including
#     the JNI bridge, consumed by the :engine:native Gradle module.
cmake_minimum_required(VERSION 3.22)
project(parallax_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PARALLAX_BUILD_TESTS "Build the gtest suite" OFF)
option(PARALLAX_BUILD_TOOLS "Build offline CLI tools" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- dependencies
set(EIGEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/eigen)
if(NOT EXISTS ${EIGEN_DIR}/Eigen/Core)
  message(FATAL_ERROR "Eigen not found at ${EIGEN_DIR}. See docs/BUILDING.md.")
endif()

# ------------------------------------------------------------------- engine lib
file(GLOB_RECURSE PARALLAX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(parallax_core STATIC ${PARALLAX_SOURCES})
target_include_directories(parallax_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${EIGEN_DIR})

# EIGEN_MPL2_ONLY keeps us out of the LGPL-licensed corners of Eigen, which
# matters because Parallax must stay permissively licensed end to end.
target_compile_definitions(parallax_core PUBLIC EIGEN_MPL2_ONLY)

# zlib (zlib licence, permissive) for PNG texture encoding and for the ZIP
# containers behind .usdz and .parallax. It ships with the NDK sysroot and is
# present on every host we build on, so it is a hard dependency rather than an
# optional one.
find_package(ZLIB REQUIRED)
target_link_libraries(parallax_core PUBLIC ZLIB::ZLIB)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(parallax_core PRIVATE
    -Wall -Wextra -Wno-unused-parameter -ffast-math -fno-finite-math-only)
endif()

# ------------------------------------------------------------- splat shaders
# The Gaussian splat compute shaders are compiled to SPIR-V at build time and
# embedded in the library, so there is no asset to ship, unpack or get out of
# step with the code that dispatches it. glslc comes with the NDK; the host
# build compiles them too, because a shader that no longer compiles is a
# broken build and CI should say so rather than leaving it for a device.
set(PARALLAX_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/splat/vulkan/shaders)
set(PARALLAX_SHADER_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders)
file(GLOB PARALLAX_SHADER_SOURCES CONFIGURE_DEPENDS ${PARALLAX_SHADER_DIR}/*.comp)
file(GLOB PARALLAX_SHADER_INCLUDES CONFIGURE_DEPENDS ${PARALLAX_SHADER_DIR}/*.glsl)

find_program(PARALLAX_GLSLC
  NAMES glslc
  HINTS
    ${ANDROID_NDK}/shader-tools/linux-x86_64
    ${ANDROID_NDK}/shader-tools/darwin-x86_64
    ${ANDROID_NDK}/shader-tools/windows-x86_64
    $ENV{ANDROID_NDK_HOME}/shader-tools/linux-x86_64
    $ENV{ANDROID_NDK_ROOT}/shader-tools/linux-x86_64
  DOC "SPIR-V compiler from the Android NDK or the Vulkan SDK")

if(NOT PARALLAX_GLSLC)
  if(ANDROID)
    message(FATAL_ERROR
      "glslc was not found, so the Gaussian splat compute shaders cannot be built. "
      "It ships with the NDK under shader-tools/.")
  endif()
  message(WARNING "glslc not found: skipping splat shader compilation on this host build.")
else()
  file(MAKE_DIRECTORY ${PARALLAX_SHADER_OUTPUT})
  set(PARALLAX_SPIRV_OUTPUTS "")
  foreach(shader ${PARALLAX_SHADER_SOURCES})
    get_filename_component(shader_name ${shader} NAME_WE)
    set(spirv ${PARALLAX_SHADER_OUTPUT}/${shader_name}.spv.inc)
    add_custom_command(
      OUTPUT ${spirv}
      COMMAND ${PARALLAX_GLSLC} --target-env=vulkan1.1 -O -mfmt=c
              -I${PARALLAX_SHADER_DIR} -o ${spirv} ${shader}
      DEPENDS ${shader} ${PARALLAX_SHADER_INCLUDES}
      COMMENT "Compiling ${shader_name}.comp to SPIR-V"
      VERBATIM)
    list(APPEND PARALLAX_SPIRV_OUTPUTS ${spirv})
  endforeach()
  add_custom_target(parallax_splat_shaders DEPENDS ${PARALLAX_SPIRV_OUTPUTS})
  add_dependencies(parallax_core parallax_splat_shaders)
  target_include_directories(parallax_core PRIVATE ${PARALLAX_SHADER_OUTPUT})
  target_compile_definitions(parallax_core PRIVATE PARALLAX_HAVE_SPLAT_SHADERS=1)
endif()

if(ANDROID)
  # Shared JNI library, built from the static core plus the bridge.
  add_library(parallax_engine SHARED ${CMAKE_CURRENT_SOURCE_DIR}/jni/ParallaxJni.cpp)
  target_link_libraries(parallax_engine PRIVATE parallax_core log android)
  target_link_options(parallax_engine PRIVATE
    "LINKER:--gc-sections" "LINKER:--exclude-libs,ALL")
endif()

# ------------------------------------------------------------------------ tests
if(PARALLAX_BUILD_TESTS)
  enable_testing()
  set(GTEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  add_subdirectory(${GTEST_DIR} ${CMAKE_CURRENT_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)

  file(GLOB PARALLAX_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(parallax_tests ${PARALLAX_TEST_SOURCES})
  target_link_libraries(parallax_tests PRIVATE parallax_core gtest gtest_main)
  target_include_directories(parallax_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME parallax_tests COMMAND parallax_tests)
endif()

# ------------------------------------------------------------------------ tools
if(PARALLAX_BUILD_TOOLS)
  file(GLOB PARALLAX_TOOL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp)
  foreach(tool_src ${PARALLAX_TOOL_SOURCES})
    get_filename_component(tool_name ${tool_src} NAME_WE)
    add_executable(${tool_name} ${tool_src})
    target_link_libraries(${tool_name} PRIVATE parallax_core)
  endforeach()
endif()
