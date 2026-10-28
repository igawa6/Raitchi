cmake_minimum_required(VERSION 3.22)
project(letsgo_android_reference C CXX)

# A host-port reference, retaining Let's Go's own ABI3 runtime/renderer.
# All dependencies are supplied from the verified local LAS build, offline.
foreach(required RECOMP_SOURCE RECOMP_ANDROID_DEPS RECOMP_FFMPEG_PREFIX RECOMP_SDL_SOURCE RECOMP_SDL_LIBRARY RECOMP_GENERATED_INCLUDE RECOMP_GAME_ID RECOMP_ADRENOTOOLS_INCLUDE RECOMP_ADRENOTOOLS_LIBRARY RECOMP_ADRENOTOOLS_NS_LIBRARY)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing ${required}; use tools/build_letsgo_android.py")
  endif()
endforeach()
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "This reference requires Android ARM64")
endif()
set(ROOT ${RECOMP_SOURCE})
set(DEPS ${RECOMP_ANDROID_DEPS})
set(FFMPEG ${RECOMP_FFMPEG_PREFIX})
set(IMGUI ${ROOT}/third_party/imgui)
add_library(SDL3 SHARED IMPORTED)
set_target_properties(SDL3 PROPERTIES IMPORTED_LOCATION "${RECOMP_SDL_LIBRARY}")

file(GLOB RT_SRC CONFIGURE_DEPENDS ${ROOT}/runtime/*.c ${ROOT}/runtime/hle/*.c
     ${ROOT}/runtime/services/*.c ${ROOT}/runtime/nvn/*.c
     ${ROOT}/runtime/nvn/maxwell/*.c ${ROOT}/runtime/nvn/vulkan/*.c)
file(GLOB RT_CXX CONFIGURE_DEPENDS ${ROOT}/runtime/nvn/vulkan/*.cpp)
file(GLOB UI_SRC CONFIGURE_DEPENDS ${ROOT}/runtime/ui/*.mm)
set_source_files_properties(${UI_SRC} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;c++")
# The pinned FFX header uses __declspec even for its unavailable non-Windows stub.
set_source_files_properties(${ROOT}/runtime/nvn/vulkan/fsr3.cpp PROPERTIES COMPILE_OPTIONS "-fdeclspec")
add_library(main SHARED ${RT_SRC} ${RT_CXX} ${UI_SRC}
    ${ROOT}/runtime/platform/android.c ${DEPS}/include/volk/volk.c
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)
target_include_directories(main PRIVATE ${ROOT}/runtime ${ROOT}/runtime/nvn
    ${IMGUI} ${IMGUI}/backends
    ${ROOT}/third_party/smaa/Textures ${RECOMP_GENERATED_INCLUDE})
target_include_directories(main SYSTEM PRIVATE ${ROOT}/third_party/fidelityfx/include
    ${RECOMP_SDL_SOURCE}/include ${FFMPEG}/include ${DEPS}/include ${DEPS}/include/volk ${RECOMP_ADRENOTOOLS_INCLUDE})
target_compile_definitions(main PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR
    IMGUI_IMPL_VULKAN_USE_VOLK "GAMELIB_ID=\"${RECOMP_GAME_ID}\""
    "GAMELIB_SHARED=\"-shared\""
    "GAMELIB_CFLAGS=\"-O1 -fPIC -fvisibility=hidden -fsigned-char -fno-strict-aliasing\"")
target_compile_options(main PRIVATE -fsigned-char -fno-strict-aliasing
    "-ffile-prefix-map=${ROOT}/=" -Wall -Wno-unused-function -Wno-format
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
target_link_options(main PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-Bsymbolic"
    "-Wl,--exclude-libs,ALL" "-Wl,-z,nostart-stop-gc" "-Wl,--no-undefined")
target_link_libraries(main PRIVATE SDL3 ${FFMPEG}/lib/libavformat.a ${FFMPEG}/lib/libavcodec.a
    ${FFMPEG}/lib/libswscale.a ${FFMPEG}/lib/libswresample.a ${FFMPEG}/lib/libavutil.a ${DEPS}/lib/libshaderc_combined.a
    ${RECOMP_ADRENOTOOLS_LIBRARY} ${RECOMP_ADRENOTOOLS_NS_LIBRARY} android log m dl)
