project(media)

# 平台层由构建系统选目录，源码里不写 #ifdef（docs/06 §6）。
# windows/：采集 + 渲染；linux/：采集（ALSA + V4L2），渲染未实现。
if(WIN32)
    set(VCS_MEDIA_PLATFORM_DIR "windows")
else()
    set(VCS_MEDIA_PLATFORM_DIR "linux")
endif()

# 收集 src/ 下所有 .cpp，排除 codec（独立目标）、webrtc 后端（独立 C++ 标准）
# 与平台目录（按平台单独收集）。
file(GLOB_RECURSE ALL_SOURCES "src/*.cpp")
list(FILTER ALL_SOURCES EXCLUDE REGEX "/src/codec/")
list(FILTER ALL_SOURCES EXCLUDE REGEX "/src/backend/webrtc/")
list(FILTER ALL_SOURCES EXCLUDE REGEX "/src/platform/")

file(GLOB PLATFORM_SOURCES "src/platform/${VCS_MEDIA_PLATFORM_DIR}/*.cpp")
if(NOT PLATFORM_SOURCES)
    message(FATAL_ERROR
        "media: no sources under src/platform/${VCS_MEDIA_PLATFORM_DIR} for this platform")
endif()

file(GLOB_RECURSE HEADERS "include/*.h" "src/*.h")

add_library(media STATIC ${ALL_SOURCES} ${PLATFORM_SOURCES} ${HEADERS})

target_include_directories(media
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${VCS_MEDIA_PLATFORM_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include/codec
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codec
        ${CMAKE_SOURCE_DIR}/base/include
        ${CMAKE_SOURCE_DIR}/common/include
)

# ---------- FFmpeg 依赖 ----------
# vcs_find_ffmpeg_lib / vcs_find_ffmpeg_include 定义在根 CMakeLists（全仓统一口径）。
set(FFMPEG_LIB_NAMES avcodec avformat avutil swscale swresample)
foreach(lib ${FFMPEG_LIB_NAMES})
    vcs_find_ffmpeg_lib(FFMPEG_${lib}_LIB ${lib})
    list(APPEND FFMPEG_LIBS ${FFMPEG_${lib}_LIB})
endforeach()

vcs_find_ffmpeg_include(FFMPEG_INC_DIR)

add_library(media_ffmpeg_dep INTERFACE)
target_include_directories(media_ffmpeg_dep INTERFACE ${FFMPEG_INC_DIR})
target_link_libraries(media_ffmpeg_dep INTERFACE ${FFMPEG_LIBS})

# 添加 codec 子目录（独立动态库）
add_subdirectory(src/codec)

# 链接基础库和 codec 库
target_link_libraries(media PUBLIC base common codec_soft media_ffmpeg_dep)

# 标准产物：libwebrtc 后端独立编译（自带 C++ 标准与 include），media 其余部分
# 跟随根基线 C++14。唯一装配点 BackendFactory::CreateDefaultBackend() 据此选择。
if(VCS_WITH_WEBRTC)
    target_compile_definitions(media PRIVATE VCS_WITH_WEBRTC=1)
    add_subdirectory(src/backend/webrtc)
    target_link_libraries(media PUBLIC media_backend_webrtc)
endif()

if(WIN32)
    target_link_libraries(media PRIVATE
        mf mfplat mfreadwrite mfuuid  # MediaFoundation
        d3d11 dxgi                    # DirectX (ScreenCapture)
        ole32 uuid                    # COM & WASAPI
        winmm                         # 多媒体
    )
    target_compile_definitions(media PRIVATE
        NOMINMAX
        _ENABLE_EXTENDED_ALIGNED_STORAGE
        WIN32_LEAN_AND_MEAN
    )
else()
    # Linux 采集：ALSA 麦克风（V4L2 摄像头只需要内核头，无额外库）。
    find_path(ALSA_INCLUDE_DIR alsa/asoundlib.h)
    find_library(ALSA_LIBRARY NAMES asound)
    if(NOT ALSA_INCLUDE_DIR OR NOT ALSA_LIBRARY)
        message(FATAL_ERROR
            "media: ALSA headers/libs not found (Linux capture backend). "
            "Install them with: sudo apt install libasound2-dev")
    endif()
    target_include_directories(media PRIVATE ${ALSA_INCLUDE_DIR})
    target_link_libraries(media PRIVATE ${ALSA_LIBRARY})
endif()
