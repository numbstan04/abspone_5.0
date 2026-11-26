project(absphone)

file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.h" "include/*.hpp" "src/*.h" "src/*.hpp")

add_library(absphone SHARED ${SOURCES} ${HEADERS})
target_compile_definitions(absphone PRIVATE VCS_ABSPHONE_EXPORTS)

target_include_directories(absphone
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# 依赖边只声明 conference/common/base：gateway、media、codec_soft、FFmpeg
# 由 conference/media 以 PUBLIC 传递。absphone 自身不引用 FFmpeg / x264 / SDL2，
# 因此不再重复链接——原来那几条依赖是绝对路径，在非本机环境里不成立。
target_link_libraries(absphone PRIVATE conference common base)

if(MSVC)
    target_link_options(absphone PRIVATE "/SAFESEH:NO")
    target_link_libraries(absphone PUBLIC
        ws2_32.lib winmm.lib ole32.lib secur32.lib crypt32.lib iphlpapi.lib)
else()
    if(NOT APPLE)
        # GNU ld 默认丢弃未被直接引用的共享库；absphone 通过 conference 间接
        # 使用 codec_soft，这里保留 DT_NEEDED。ld64 无此开关，也不需要它。
        target_link_options(absphone PRIVATE -Wl,--no-as-needed)
    endif()
    # AppPaths 用 boost::dll::program_location -> dladdr；老 glibc 需要 -ldl。
    target_link_libraries(absphone PRIVATE ${CMAKE_DL_LIBS})
    # 安装后与 base/codec_soft 同目录：ELF 用 $ORIGIN，Mach-O 用 @loader_path。
    set_target_properties(absphone PROPERTIES
        INSTALL_RPATH "${VCS_ORIGIN}")
endif()

install(TARGETS absphone
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
