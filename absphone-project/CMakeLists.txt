# 3.13: target_link_directories (gateway / codec / absphone 都在用)。
cmake_minimum_required(VERSION 3.13)
project(EP_AbsPhone VERSION 1.0.0 LANGUAGES CXX)

# ============================================================
# 平台编译开关：只允许出现在构建系统，源码与公共头零平台宏（docs/04 §3.1）
# ============================================================
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()
if(MSVC)
    add_compile_definitions(_DISABLE_EXTENDED_ALIGNED_STORAGE)
endif()
if(NOT WIN32)
    add_compile_definitions(_GNU_SOURCE)
endif()

# 基线 C++14：MSVC 本来就没有 C++11 模式，代码里也用到了 std::make_unique
# （C++14）。公共头仍保持 C++11 可用，不强迫调用方升级标准。
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 线程库由根统一查找：base 用 std::thread / Boost.Asio，GCC 下必须 -pthread。
# 放在根上是为了让所有子目录都拿到同一个导入目标，而不是各自 find。
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem system)

# FFmpeg 查找口径全仓统一：给了 FFMPEG_ROOT 就只在该树下找（Windows 预编译包
# / 自定义构建），没给就走系统路径（Linux/macOS 的发行版包）。media 与 codec
# 共用，避免两处各写一套 NO_DEFAULT_PATH。
# 不用 find_library(REQUIRED)：那是 3.18 才有的关键字，本工程最低 3.13。
function(vcs_find_ffmpeg_lib out_name lib)
    if(FFMPEG_ROOT)
        find_library(${out_name} NAMES ${lib}
            PATHS
                ${FFMPEG_ROOT}/lib
                ${FFMPEG_ROOT}/lib${lib}
                ${FFMPEG_ROOT}/lib${lib}/.libs
                ${FFMPEG_ROOT}/build/lib
                ${FFMPEG_ROOT}/.libs
            NO_DEFAULT_PATH)
    else()
        find_library(${out_name} NAMES ${lib})
    endif()
    if("${${out_name}}" STREQUAL "${out_name}-NOTFOUND")
        message(FATAL_ERROR
            "FFmpeg library '${lib}' not found. Install the FFmpeg dev package "
            "or configure with -DFFMPEG_ROOT=<prefix>.")
    endif()
endfunction()

function(vcs_find_ffmpeg_include out_var)
    if(FFMPEG_ROOT)
        find_path(${out_var} NAMES libavcodec/avcodec.h
            PATHS
                ${FFMPEG_ROOT}/include
                ${FFMPEG_ROOT}
                ${FFMPEG_ROOT}/libavcodec
            NO_DEFAULT_PATH)
    else()
        find_path(${out_var} NAMES libavcodec/avcodec.h)
    endif()
    if("${${out_var}}" STREQUAL "${out_var}-NOTFOUND")
        message(FATAL_ERROR
            "FFmpeg headers not found (libavcodec/avcodec.h). Install the "
            "FFmpeg dev package or configure with -DFFMPEG_ROOT=<prefix>.")
    endif()
endfunction()

# Sources are UTF-8. Tell MSVC explicitly, otherwise it decodes them with the
# system ANSI code page (936) and mis-parses trailing comment bytes.
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(PJSIP_ROOT "" CACHE PATH "PJSIP source root")
set(PJSIP_LIB_DIR "" CACHE PATH "Directory holding pjproject archives, when not under <PJSIP_ROOT>/<module>/lib")
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg source root")
# OpenH264：pjmedia-codec 的 H.264 编解码器实现是外部库，pjproject 归档只留
# Wels* 未定义符号。Windows 预编译归档确实引用它们；POSIX 归档当前未引用。
set(OPENH264_ROOT "" CACHE PATH "OpenH264 prefix (lib/openh264.{lib,dll} or .a/.so)")
if(NOT OPENH264_ROOT)
    # 仓库既有的相对布局：<repo>/../third_party/openh264-*。找不到就交给
    # find_library 走系统路径（POSIX 的发行版包）。
    file(GLOB _openh264_siblings "${CMAKE_SOURCE_DIR}/../third_party/openh264-*")
    list(SORT _openh264_siblings)
    list(REVERSE _openh264_siblings)
    if(_openh264_siblings)
        list(GET _openh264_siblings 0 OPENH264_ROOT)
    endif()
endif()
set(X264_ROOT "" CACHE PATH "x264 source root")
set(X265_ROOT "" CACHE PATH "x265 source root")

# ---- Media backend artifact (docs/12 §9, §15 P0-1) ----
# Build-time selection only: the artifact is chosen when configuring, there is
# no runtime backend switch in the binary.
#   OFF (default) = lite   : NativeRtp only, no WebRTC headers or libraries.
#   ON            = standard: libwebrtc backend target (P4).
option(VCS_WITH_WEBRTC "Build the libwebrtc-backed RTP media backend (standard artifact)" OFF)
if(VCS_WITH_WEBRTC)
    set(VCS_MEDIA_ARTIFACT "standard")
else()
    set(VCS_MEDIA_ARTIFACT "lite")
endif()
message(STATUS "Media artifact: ${VCS_MEDIA_ARTIFACT} (VCS_WITH_WEBRTC=${VCS_WITH_WEBRTC})")

if(VCS_WITH_WEBRTC AND NOT EXISTS "${CMAKE_SOURCE_DIR}/media/src/backend/webrtc/CMakeLists.txt")
    message(FATAL_ERROR
        "VCS_WITH_WEBRTC=ON requires media/src/backend/webrtc/ (P4, docs/12 §11). "
        "Configure with -DVCS_WITH_WEBRTC=OFF (default) until that backend lands.")
endif()

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
# 不设置 CMAKE_SKIP_BUILD_RPATH：Linux 构建树里的可执行文件需要自动 RPATH
# 才能找到同一次构建出的 .so；Windows 不使用 RPATH，不受影响。

# 运行期相对搜索路径的口径：ELF 用 $ORIGIN，Mach-O 用 @loader_path。
# 只在构建系统里分叉，业务代码不感知。
if(APPLE)
    set(VCS_ORIGIN "@loader_path")
else()
    set(VCS_ORIGIN "\$ORIGIN")
endif()

add_subdirectory(base)
add_subdirectory(common)
add_subdirectory(ai)
add_subdirectory(media)      # 内部会 add_subdirectory(src/codec)
add_subdirectory(gateway)
add_subdirectory(conference)
add_subdirectory(absphone)
add_subdirectory(tests)