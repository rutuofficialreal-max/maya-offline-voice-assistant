cmake_minimum_required(VERSION 3.22.1)
project(maya LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TP "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
option(MAYA_VULKAN "Build llama.cpp's Vulkan (GPU) backend" ON)

foreach(dep llama.cpp whisper.cpp)
  if(NOT EXISTS "${TP}/${dep}/CMakeLists.txt")
    message(FATAL_ERROR "third_party/${dep} is missing - run scripts/fetch_deps.sh first")
  endif()
endforeach()
if(NOT EXISTS "${TP}/sherpa-onnx/jniLibs/${ANDROID_ABI}/libsherpa-onnx-c-api.so")
  message(FATAL_ERROR "sherpa-onnx prebuilt libraries missing for ${ANDROID_ABI} - run scripts/fetch_deps.sh")
endif()
include("${TP}/versions.cmake" OPTIONAL)
if(NOT MAYA_WHISPER_CPP_TAG)
  set(MAYA_WHISPER_CPP_TAG "unknown")
endif()

# ------------------------------------------------------------------------------------------
# llama.cpp + ggml. There is exactly ONE copy of ggml in the final library: whisper.cpp's
# source file is compiled below against *this* ggml, so LLM and STT share backends and there
# are no duplicate-symbol problems.
# ------------------------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)      # cross-compiling: never probe the build machine
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)      # ggml has its own thread pool; OpenMP adds nothing on Android
set(GGML_LLAMAFILE OFF CACHE BOOL "" FORCE)
set(GGML_CPU_KLEIDIAI OFF CACHE BOOL "" FORCE)
if(ANDROID_ABI STREQUAL "arm64-v8a")
  # Snapdragon 4 Gen 5: Cortex-A78 + A55 = ARMv8.2-A with dot-product and fp16 (no i8mm / SVE).
  set(GGML_CPU_ARM_ARCH "armv8.2-a+dotprod+fp16" CACHE STRING "" FORCE)
endif()

if(MAYA_VULKAN)
  set(GGML_VULKAN ON CACHE BOOL "" FORCE)
  # vulkan.hpp is not in every NDK; use the Khronos headers fetched by scripts/fetch_deps.sh.
  set(Vulkan_INCLUDE_DIR "${TP}/Vulkan-Headers/include" CACHE PATH "" FORCE)
  set(SPIRV-Headers_DIR "${TP}/spirv-headers-install/share/cmake/SPIRV-Headers" CACHE PATH "" FORCE)
  # The NDK ships both the libvulkan.so stub and the glslc shader compiler.
  if(NOT Vulkan_GLSLC_EXECUTABLE AND ANDROID_NDK AND ANDROID_HOST_TAG)
    set(_glslc "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}/glslc")
    if(EXISTS "${_glslc}")
      set(Vulkan_GLSLC_EXECUTABLE "${_glslc}" CACHE FILEPATH "" FORCE)
    endif()
  endif()
  if(NOT Vulkan_LIBRARY AND CMAKE_SYSROOT AND ANDROID_ABI STREQUAL "arm64-v8a")
    set(_vklib "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android/${ANDROID_PLATFORM_LEVEL}/libvulkan.so")
    if(EXISTS "${_vklib}")
      set(Vulkan_LIBRARY "${_vklib}" CACHE FILEPATH "" FORCE)
    endif()
  endif()
  message(STATUS "Maya: Vulkan backend ON (glslc=${Vulkan_GLSLC_EXECUTABLE}, libvulkan=${Vulkan_LIBRARY})")
else()
  set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
  message(STATUS "Maya: Vulkan backend OFF (CPU-only LLM)")
endif()

add_subdirectory("${TP}/llama.cpp" llama_build EXCLUDE_FROM_ALL)

# ggml-vulkan includes <spirv/unified1/spirv.hpp> directly, but its CMakeLists only does
# find_package(SPIRV-Headers CONFIG) without attaching the headers to the target, so the
# include directory has to be added here (this file is why: ggml-vulkan-types.h:47).
if(MAYA_VULKAN AND TARGET ggml-vulkan)
  target_include_directories(ggml-vulkan PRIVATE "${TP}/SPIRV-Headers/include")
endif()

# ------------------------------------------------------------------------------------------
# sherpa-onnx prebuilt C API + ONNX Runtime (wake word, Silero VAD, VITS TTS)
# ------------------------------------------------------------------------------------------
set(SHERPA "${TP}/sherpa-onnx")
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
  IMPORTED_LOCATION "${SHERPA}/jniLibs/${ANDROID_ABI}/libonnxruntime.so")
add_library(sherpa-onnx-c-api SHARED IMPORTED)
set_target_properties(sherpa-onnx-c-api PROPERTIES
  IMPORTED_LOCATION "${SHERPA}/jniLibs/${ANDROID_ABI}/libsherpa-onnx-c-api.so"
  INTERFACE_INCLUDE_DIRECTORIES "${SHERPA}/include"
  INTERFACE_LINK_LIBRARIES onnxruntime)

# ------------------------------------------------------------------------------------------
# libmaya.so
# ------------------------------------------------------------------------------------------
# whisper.cpp's single translation unit plus the optional-encoder hook it always references
# (aneforge is dlopen-based and inert unless an env var is set; it just has to link).
set(WHISPER_SRC
  "${TP}/whisper.cpp/src/whisper.cpp"
  "${TP}/whisper.cpp/src/aneforge/whisper-aneforge.cpp")
add_library(maya SHARED
  src/maya_jni.cpp
  src/engine.cpp
  src/audio_capture.cpp
  src/audio_player.cpp
  src/wake_word.cpp
  src/vad.cpp
  src/stt_whisper.cpp
  src/llm.cpp
  src/tts_sherpa.cpp
  src/text_utils.cpp
  src/cpu_affinity.cpp
  ${WHISPER_SRC})

set_source_files_properties(${WHISPER_SRC} PROPERTIES
  COMPILE_DEFINITIONS "WHISPER_VERSION=\"${MAYA_WHISPER_CPP_TAG}\"")

target_include_directories(maya PRIVATE
  src
  "${TP}/whisper.cpp/include"
  "${TP}/whisper.cpp/src")

target_compile_options(maya PRIVATE -O3 -fvisibility=hidden -ffunction-sections -fdata-sections -Wall -Wno-unused-parameter)
target_link_options(maya PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)

# llama pulls in ggml (CPU + Vulkan) transitively and exports their include directories.
target_link_libraries(maya PRIVATE llama sherpa-onnx-c-api onnxruntime android log aaudio)
