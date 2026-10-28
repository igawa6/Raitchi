cmake_minimum_required(VERSION 3.22)
project(shared_android_runtime C CXX ASM)

# One Android runtime, supplied entirely from staged pinned source and cached dependencies.
# No title, guest shift, hook profile or generated game code is compiled into this target.
foreach(required RECOMP_SOURCE RECOMP_ANDROID_DEPS RECOMP_FFMPEG_PREFIX RECOMP_SDL_SOURCE
                 RECOMP_SDL_LIBRARY RECOMP_GENERATED_INCLUDE RECOMP_OVERLAY_INDEX
                 RECOMP_ADRENOTOOLS_INCLUDE RECOMP_ADRENOTOOLS_LIBRARY RECOMP_ADRENOTOOLS_NS_LIBRARY)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing ${required}; use tools/build_shared_android.py")
  endif()
endforeach()
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "The common Android runtime requires arm64-v8a")
endif()
set(ROOT "${RECOMP_SOURCE}")
set(DEPS "${RECOMP_ANDROID_DEPS}")
set(FFMPEG "${RECOMP_FFMPEG_PREFIX}")
set(IMGUI "${ROOT}/third_party/imgui")
include("${RECOMP_OVERLAY_INDEX}")
add_library(SDL3 SHARED IMPORTED)
set_target_properties(SDL3 PROPERTIES IMPORTED_LOCATION "${RECOMP_SDL_LIBRARY}")

file(GLOB RT_SRC CONFIGURE_DEPENDS "${ROOT}/runtime/*.c" "${ROOT}/runtime/hle/*.c"
     "${ROOT}/runtime/services/*.c" "${ROOT}/runtime/nvn/*.c"
     "${ROOT}/runtime/nvn/maxwell/*.c" "${ROOT}/runtime/nvn/vulkan/*.c")
# Replace these implementations only when the staged overlay explicitly supplies them.
# The common core owns rt_dispatch/rt_hook, profiler globals/hit and libc wrapper bodies.
foreach(legacy dispatch.c main.c gamelib.c fnprof.c)
  list(REMOVE_ITEM RT_SRC "${ROOT}/runtime/${legacy}")
  if("${legacy}" IN_LIST RECOMP_RUNTIME_OVERLAYS)
    list(APPEND RT_SRC "${ROOT}/runtime/${legacy}")
  endif()
endforeach()
list(REMOVE_ITEM RT_SRC "${ROOT}/runtime/hle/libc.c")
list(REMOVE_ITEM RT_SRC "${ROOT}/runtime/native_package_prepare_android.c")
list(REMOVE_ITEM RT_SRC "${ROOT}/runtime/native_package_metadata.c")
# The Unicorn-based native-blocks backend is a diagnostic alternative to
# continuous NCE. Unicorn is GPL-2.0, so it is opt-in.
option(RAITCHI_NATIVE_BLOCKS "Build the Unicorn native-blocks diagnostic backend" OFF)
if(RAITCHI_NATIVE_BLOCKS)
  list(REMOVE_ITEM RT_SRC "${ROOT}/runtime/native_engine_disabled.c")
  set(RAITCHI_UNICORN_INCLUDE "${RECOMP_UNICORN_INCLUDE}")
  set(RAITCHI_UNICORN_LIBRARY "${RECOMP_UNICORN_LIBRARY}")
else()
  list(REMOVE_ITEM RT_SRC "${ROOT}/runtime/native_engine.c")
  set(RAITCHI_UNICORN_INCLUDE "")
  set(RAITCHI_UNICORN_LIBRARY "")
endif()
file(GLOB RT_CXX CONFIGURE_DEPENDS "${ROOT}/runtime/*.cpp" "${ROOT}/runtime/nvn/vulkan/*.cpp")
file(GLOB RT_ASM CONFIGURE_DEPENDS "${ROOT}/runtime/*.S")
file(GLOB UI_SRC CONFIGURE_DEPENDS "${ROOT}/runtime/ui/*.mm")
set_source_files_properties(${UI_SRC} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;c++")
file(GLOB COMMON_SRC CONFIGURE_DEPENDS "${ROOT}/native/*.c" "${ROOT}/native/runtime/*.c"
     "${ROOT}/native/native-game-backend/*.c"
     "${ROOT}/native/continuous-nce/*.c"
     "${ROOT}/native/adapters/*.c" "${ROOT}/native/adapters/las/*.c"
     "${ROOT}/native/adapters/letsgo/*.c" "${ROOT}/native/adapters/echoes/*.c"
     "${ROOT}/native/adapters/wonder/*.c" "${ROOT}/native/adapters/generic/*.c")
file(GLOB COMMON_CXX CONFIGURE_DEPENDS "${ROOT}/native/runtime/*.cpp"
     "${ROOT}/native/adapters/*.cpp" "${ROOT}/native/adapters/las/*.cpp"
     "${ROOT}/native/adapters/letsgo/*.cpp")
add_library(main SHARED ${RT_SRC} ${RT_CXX} ${RT_ASM} ${UI_SRC} ${COMMON_SRC} ${COMMON_CXX}
    "${ROOT}/runtime/platform/android.c" "${ROOT}/runtime/platform/android_jni.c"
    "${DEPS}/include/volk/volk.c"
    "${IMGUI}/imgui.cpp" "${IMGUI}/imgui_draw.cpp" "${IMGUI}/imgui_tables.cpp" "${IMGUI}/imgui_widgets.cpp"
    "${IMGUI}/backends/imgui_impl_sdl3.cpp" "${IMGUI}/backends/imgui_impl_vulkan.cpp")
set_target_properties(main PROPERTIES OUTPUT_NAME main)
set_property(TARGET main APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
target_include_directories(main PRIVATE "${ROOT}/runtime" "${ROOT}/runtime/nvn"
    "${ROOT}/native" "${ROOT}/native/runtime" "${ROOT}/native/adapters"
    "${IMGUI}" "${IMGUI}/backends" "${ROOT}/third_party/smaa/Textures" "${RECOMP_GENERATED_INCLUDE}")
target_include_directories(main SYSTEM PRIVATE "${RECOMP_SDL_SOURCE}/include" "${FFMPEG}/include"
    "${DEPS}/include" "${DEPS}/include/volk" "${RECOMP_ADRENOTOOLS_INCLUDE}"
    "${RECOMP_CAPSTONE_INCLUDE}" ${RAITCHI_UNICORN_INCLUDE})

# Keep shader caches separate when translator/backend inputs change, like pinned LAS Android.
set(TRANSLATOR_CAT "")
foreach(file runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h
             runtime/nvn/maxwell/direct_spirv.c runtime/nvn/maxwell/direct_spirv.h
             runtime/nvn/maxwell/shader_compiler.c runtime/nvn/maxwell/shader_compiler.h
             runtime/nvn/vulkan/exec.c)
  file(SHA256 "${ROOT}/${file}" digest)
  string(APPEND TRANSLATOR_CAT "${digest}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ROOT}/${file}")
endforeach()
string(SHA256 TRANSLATOR_ID "${TRANSLATOR_CAT}")
string(SUBSTRING "${TRANSLATOR_ID}" 0 12 TRANSLATOR_ID)
target_compile_definitions(main PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR
    IMGUI_IMPL_VULKAN_USE_VOLK "SHADER_TRANSLATOR_ID=\"${TRANSLATOR_ID}\"")
target_compile_options(main PRIVATE -fsigned-char -fno-strict-aliasing -ffixed-x18
    "-ffile-prefix-map=${ROOT}/=" -Wall -Wno-unused-function -Wno-format
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
# Dynamic runtime/core/HLE exports intentionally remain available to the local title DSO.
# Hide bundled static dependency exports and retain section-based registries.
target_link_options(main PRIVATE "-Wl,-soname,libmain.so" "-Wl,-z,max-page-size=16384"
    "-Wl,-Bsymbolic" "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map" "-Wl,--exclude-libs,ALL" "-Wl,-z,nostart-stop-gc" "-Wl,--no-undefined")
target_link_libraries(main PRIVATE SDL3 "${FFMPEG}/lib/libavformat.a" "${FFMPEG}/lib/libavcodec.a"
    "${FFMPEG}/lib/libswscale.a" "${FFMPEG}/lib/libswresample.a" "${FFMPEG}/lib/libavutil.a"
    "${DEPS}/lib/libshaderc_combined.a" "${RECOMP_ADRENOTOOLS_LIBRARY}"
    "${RECOMP_ADRENOTOOLS_NS_LIBRARY}" "${RECOMP_CAPSTONE_LIBRARY}" ${RAITCHI_UNICORN_LIBRARY} android log m dl)

# Launcher preparation is separate from the game process and renderer. Only
# original ExeFS metadata and the package reader are needed; no guest code runs.
add_library(raitchiprepare SHARED
    "${ROOT}/runtime/native_package_prepare_android.c" "${ROOT}/runtime/nxpkg.c"
    "${ROOT}/runtime/shared_hash.cpp" "${ROOT}/native/runtime/native_prepare.c"
    "${ROOT}/native/module_abi.c" "${ROOT}/native/runtime/native_manifest.c"
    "${ROOT}/native/runtime/nso_image.c" "${ROOT}/native/runtime/session_layout.c"
    "${ROOT}/native/adapters/title_adapter.c" "${ROOT}/native/adapters/las/profile.c"
    "${ROOT}/native/adapters/letsgo/profile.c" "${ROOT}/native/adapters/echoes/profile.c"
    "${ROOT}/native/adapters/wonder/profile.c" "${ROOT}/native/adapters/generic/generic.c"
    "${ROOT}/runtime/native_package_metadata.c")
target_include_directories(raitchiprepare PRIVATE "${ROOT}/runtime" "${ROOT}/native"
    "${ROOT}/native/runtime" "${ROOT}/native/adapters" "${RECOMP_GENERATED_INCLUDE}")
target_compile_options(raitchiprepare PRIVATE -fsigned-char -fno-strict-aliasing -ffixed-x18
    -fvisibility=hidden "-ffile-prefix-map=${ROOT}/=" -Wall
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
target_link_options(raitchiprepare PRIVATE "-Wl,-soname,libraitchiprepare.so"
    "-Wl,-z,max-page-size=16384" "-Wl,--exclude-libs,ALL" "-Wl,--no-undefined")
