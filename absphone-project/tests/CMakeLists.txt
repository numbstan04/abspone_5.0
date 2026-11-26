project(tests)

# 从父作用域获取必要的变量（已在根 CMakeLists.txt 中定义）
if(NOT PJSIP_ROOT)
    set(PJSIP_ROOT ${PJSIP_ROOT} CACHE PATH "PJSIP source root")
endif()
if(NOT FFMPEG_ROOT)
    set(FFMPEG_ROOT ${FFMPEG_ROOT} CACHE PATH "FFmpeg source root")
endif()

if(NOT X264_ROOT)
    set(X264_ROOT ${X264_ROOT} CACHE PATH "x264 source root")
endif()

if(NOT X265_ROOT)
    set(X265_ROOT ${X265_ROOT} CACHE PATH "x265 source root")
endif()

if(NOT BOOST_ROOT)
	set(BOOST_ROOT ${BOOST_ROOT} CACHE PATH "boost source root")
endif()

if(NOT CMAKE_BINARY_DIR)
    # CMAKE_BINARY_DIR 是构建目录的根，通常自动定义，无需手动设置
endif()

add_executable(endpoint_demo endpoint_demo.cpp)

# 设置 -rpath-link 用于链接时查找依赖
set(_POSSIBLE_RPATHS
    ${PJSIP_ROOT}/lib
    ${PJSIP_ROOT}/pjlib/lib
    ${PJSIP_ROOT}/pjsip/lib
    ${PJSIP_ROOT}/pjmedia/lib
    ${PJSIP_ROOT}/pjnath/lib
    ${PJSIP_ROOT}/pjlib-util/lib
    ${PJSIP_ROOT}/build/lib
	${PJSIP_ROOT}/third_party/lib
    ${FFMPEG_ROOT}/lib
    ${FFMPEG_ROOT}/libavcodec
    ${FFMPEG_ROOT}/libavformat
    ${FFMPEG_ROOT}/libavutil
    ${FFMPEG_ROOT}/libswscale
    ${FFMPEG_ROOT}/libswresample
    ${FFMPEG_ROOT}/build/lib
    ${X264_ROOT}/local/lib
    ${X265_ROOT}/local/lib
    ${BOOST_ROOT}/stage/lib
    ${CMAKE_BINARY_DIR}/media/src/codec
)
foreach(_dir ${_POSSIBLE_RPATHS})
    if(EXISTS ${_dir})
        list(APPEND _RPATH_DIRS ${_dir})
    endif()
endforeach()
list(REMOVE_DUPLICATES _RPATH_DIRS)
# -Wl,... is a GNU ld flag; MSVC rejects it (LNK4044) and resolves
# dependencies through the import libraries and PATH instead. ld64 (macOS)
# does not support -rpath-link at all.
if(_RPATH_DIRS AND UNIX AND NOT APPLE)
    string(REPLACE ";" ":" _RPATH_LINK_STRING "${_RPATH_DIRS}")
    target_link_options(endpoint_demo PRIVATE -Wl,-rpath-link,${_RPATH_LINK_STRING})
endif()

target_include_directories(endpoint_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/base/include
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/absphone/include
    ${BOOST_ROOT}
)

# The demo uses base types directly (Json, ILogManager), so it must link base
# itself; absphone links it PRIVATE.
target_link_libraries(endpoint_demo absphone base)

# pjmedia 的 H.264 编解码器在运行期加载 openh264.dll（Windows 预编译包）。
set(_OPENH264_RUNTIME "")
if(WIN32 AND OPENH264_ROOT AND EXISTS "${OPENH264_ROOT}/lib/openh264.dll")
    set(_OPENH264_RUNTIME "${OPENH264_ROOT}/lib/openh264.dll")
endif()

# FFmpeg is linked as shared libraries; the Windows loader needs them next to
# the executable (or on PATH). Copy the runtime DLLs into the test output dir.
set(_FFMPEG_RUNTIME_DLLS "")
if(WIN32 AND EXISTS "${FFMPEG_ROOT}/bin")
    file(GLOB _FFMPEG_RUNTIME_DLLS "${FFMPEG_ROOT}/bin/*.dll")
endif()

# base is SHARED so the process has exactly one LogManager. Copy every shared
# library next to the executable so the build tree is runnable on Linux/macOS
# too (the build-tree RPATH below only points at the executable's own dir).
add_custom_command(TARGET endpoint_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:base> $<TARGET_FILE:absphone> $<TARGET_FILE:codec_soft>
        ${_OPENH264_RUNTIME} ${_FFMPEG_RUNTIME_DLLS}
        $<TARGET_FILE_DIR:endpoint_demo>)

# 运行时搜索路径：构建树里相对路径命中上面的副本；安装后 ../lib。
set_target_properties(endpoint_demo PROPERTIES
    BUILD_RPATH "${VCS_ORIGIN}"
    INSTALL_RPATH "${VCS_ORIGIN}/../lib"
)

install(TARGETS endpoint_demo RUNTIME DESTINATION bin)

# docs/08 §5 验收 #11：注入媒体启动的五类失败，验证 SystemError 可单条定位。
# 直接驱动 conference 执行类，用假 gateway / 假 media 触发各阶段。
add_executable(media_failure_probe media_failure_probe.cpp)
target_include_directories(media_failure_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/base/include
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/gateway/include
    ${CMAKE_SOURCE_DIR}/media/include
    ${CMAKE_SOURCE_DIR}/conference/include
    ${CMAKE_SOURCE_DIR}/conference/src   # White-box test seam: only here may a test reach into a module's src
    ${BOOST_ROOT}
)
target_link_libraries(media_failure_probe conference base)
add_custom_command(TARGET media_failure_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:base> $<TARGET_FILE:codec_soft>
        ${_OPENH264_RUNTIME} ${_FFMPEG_RUNTIME_DLLS}
        $<TARGET_FILE_DIR:media_failure_probe>)
set_target_properties(media_failure_probe PROPERTIES
    BUILD_RPATH "${VCS_ORIGIN}"
    INSTALL_RPATH "${VCS_ORIGIN}/../lib")
