project(codec_soft)

file(GLOB_RECURSE SOURCES "*.cpp")
file(GLOB_RECURSE HEADERS "./*.h"
"../../include/codec/*.h"
"software/*.h"
"remote/*.h"
"hardware/*.h"
)
add_library(codec_soft SHARED ${SOURCES} ${HEADERS})
target_compile_definitions(codec_soft PRIVATE VCS_CODEC_EXPORTS)

target_include_directories(codec_soft
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/software
        ${CMAKE_CURRENT_SOURCE_DIR}/hardware
        ${CMAKE_CURRENT_SOURCE_DIR}/remote
        ${CMAKE_SOURCE_DIR}/media/include
        ${CMAKE_SOURCE_DIR}/media/include/codec
        ${CMAKE_SOURCE_DIR}/base/include
        ${CMAKE_SOURCE_DIR}/common/include
)

# FFmpeg 查找口径与 media 一致（vcs_find_ffmpeg_* 在 media/CMakeLists.txt 定义，
# 本目录是它的子目录）：给了 FFMPEG_ROOT 就只在该树下找，否则走系统路径。
set(FFMPEG_LIB_NAMES avcodec avformat avutil swscale swresample)
foreach(lib ${FFMPEG_LIB_NAMES})
    vcs_find_ffmpeg_lib(CODEC_FFMPEG_${lib}_LIB ${lib})
    list(APPEND CODEC_FFMPEG_LIBS ${CODEC_FFMPEG_${lib}_LIB})
endforeach()
vcs_find_ffmpeg_include(CODEC_FFMPEG_INC_DIR)

target_include_directories(codec_soft PRIVATE ${CODEC_FFMPEG_INC_DIR})

# base 是 CMake target：Boost / Threads 由它以 PUBLIC 传递，不再写死
# vc141 的导入库文件名（那套名字只在 MSVC 预编译包下成立）。
target_link_libraries(codec_soft PRIVATE ${CODEC_FFMPEG_LIBS} base)

# 关闭 SAFESEH（仅 MSVC）
if(MSVC)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.13")
        target_link_options(codec_soft PRIVATE "/SAFESEH:NO")
    else()
        set_target_properties(codec_soft PROPERTIES LINK_FLAGS "/SAFESEH:NO")
    endif()
endif()

# 输出名由 CMake 按平台决定：Windows codec_soft.dll，Linux libcodec_soft.so，
# macOS libcodec_soft.dylib。不再手写 PREFIX/SUFFIX（那会让 macOS 拿到 .so）。
# 安装后与 base 同目录，用相对路径找依赖（$ORIGIN / @loader_path）。
set_target_properties(codec_soft PROPERTIES
    INSTALL_RPATH "${VCS_ORIGIN}")
install(TARGETS codec_soft
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)