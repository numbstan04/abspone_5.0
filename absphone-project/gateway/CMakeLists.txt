project(gateway)

file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.h" "include/*.hpp" "src/*.h" "src/*.hpp")
add_library(gateway STATIC ${SOURCES} ${HEADERS})

target_include_directories(gateway
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/base/include
        ${CMAKE_SOURCE_DIR}/common/include
        ${PJSIP_ROOT}/pjlib/include
        ${PJSIP_ROOT}/pjsip/include
        ${PJSIP_ROOT}/pjmedia/include
        ${PJSIP_ROOT}/pjnath/include
        ${PJSIP_ROOT}/pjlib-util/include
)

# [过渡] 08 号片移除 media：静态库经接口指针虚调用 media，无需链接期符号；
# 契约头已由 common 提供。
target_link_libraries(gateway PUBLIC base common)

# OpenH264：pjproject 的 H.264 编解码器是外部实现。
#   * 官方 Windows 预编译包（vc14 解决方案）的 pjmedia-codec 归档里留有 Wels*
#     未定义符号，不链接就过不了 link；
#   * 自己用 pjproject 的 CMake 构建、并且在 pjlib/include/pj/config_site.h 里
#     把 PJMEDIA_HAS_OPENH264_CODEC 置 0 时，归档里没有这些符号，链接期不需要它。
# 所以缺库只告警不硬失败：换一套 pjproject 归档不该逼迫改构建脚本。
# 放在 gateway：它是 pjproject 的唯一持有者，依赖不该由 absphone 声明。
if(OPENH264_ROOT)
    find_library(OPENH264_LIB NAMES openh264
        PATHS ${OPENH264_ROOT}/lib ${OPENH264_ROOT}/lib/x86 ${OPENH264_ROOT}/lib/win32
        NO_DEFAULT_PATH)
else()
    find_library(OPENH264_LIB NAMES openh264)
endif()
if(OPENH264_LIB)
    message(STATUS "gateway: OpenH264 ${OPENH264_LIB}")
    target_link_libraries(gateway PUBLIC ${OPENH264_LIB})
else()
    message(STATUS
        "gateway: OpenH264 not found; assuming pjproject was built with "
        "PJMEDIA_HAS_OPENH264_CODEC=0")
endif()

if(MSVC)
    # Windows 走预编译归档。各代 pjproject 的目录布局和归档命名都要能认：
    #   * <= 2.5 的 vc14 解决方案：<root>/<module>/lib/<name>-i386-Win32-vc14-<cfg>.lib
    #   * >= 2.13 的 CMake 构建： <build>/<module>/<cfg>/<name>.lib
    #   * 新版 CMake 构建：<build>/<module>/lib/lib<name>-<cfg>.lib
    #     （pjlib 的归档名是 libpj，pjsua-lib 是 libpjsua）
    # PJSIP_LIB_DIR 给定时优先在这些目录里找（分号分隔，可给多个）。
    set(PJSIP_MSVC_LIB_DIRS
        ${PJSIP_LIB_DIR}
        ${PJSIP_ROOT}/pjsip/lib
        ${PJSIP_ROOT}/pjmedia/lib
        ${PJSIP_ROOT}/pjnath/lib
        ${PJSIP_ROOT}/pjlib-util/lib
        ${PJSIP_ROOT}/pjlib/lib
        ${PJSIP_ROOT}/third_party/lib
    )

    # 归档名后缀：vc14 预编译包带平台/工具集/配置，旧 CMake 构建不带，
    # 新版 CMake 构建用 -Debug / -Release。Debug 与 Release 都要列出，因为
    # 单配置缓存里的 CMAKE_BUILD_TYPE 与预编译归档实际存在的配置未必一致。
    set(PJSIP_MSVC_LIB_SUFFIXES
        "-i386-Win32-vc14-Debug" "-i386-Win32-vc14-Release"
        "-Debug" "-Release" "")

    # 必需模块：逻辑名 | 归档候选名（后者随 pjproject 版本变化）
    set(_pj_required
        "pjlib|pjlib|libpj"
        "pjlib-util|pjlib-util|libpjlib-util"
        "pjnath|pjnath|libpjnath"
        "pjsip|pjsip-core|libpjsip"
        "pjsip-simple|pjsip-simple|libpjsip-simple"
        "pjsip-ua|pjsip-ua|libpjsip-ua"
        "pjsua-lib|pjsua-lib|pjsua|libpjsua"
        "pjsua2|pjsua2-lib|libpjsua2"
        "pjmedia|pjmedia|libpjmedia"
        "pjmedia-codec|pjmedia-codec|libpjmedia-codec"
        "pjmedia-audiodev|pjmedia-audiodev|libpjmedia-audiodev"
        "pjmedia-videodev|pjmedia-videodev|libpjmedia-videodev"
    )

    # 第三方归档：有则链接（pjmedia 里 gsm/ilbc/speex/g7221/srtp/yuv/webrtc
    # 是外部实现），没有就交给 link 阶段报未解析符号。
    set(_pj_optional
        "srtp|libsrtp"
        "resample|libresample"
        "speex|libspeex"
        "yuv|libyuv"
        "webrtc|libwebrtc"
        "gsm|libgsmcodec"
        "ilbc|libilbccodec"
        "g7221|libg7221codec"
        "baseclasses|libbaseclasses"
        "milenage|libmilenage"
    )

    set(PJSIP_LIBS "")
    foreach(_spec IN LISTS _pj_required)
        string(REPLACE "|" ";" _names "${_spec}")
        set(_hit "")
        foreach(_name IN LISTS _names)
            foreach(_suffix IN LISTS PJSIP_MSVC_LIB_SUFFIXES)
                find_library(PJSIP_${_name}${_suffix}_LIB NAMES "${_name}${_suffix}"
                    PATHS ${PJSIP_MSVC_LIB_DIRS} NO_DEFAULT_PATH)
                if(NOT _hit AND PJSIP_${_name}${_suffix}_LIB)
                    set(_hit "${PJSIP_${_name}${_suffix}_LIB}")
                endif()
            endforeach()
        endforeach()
        if(NOT _hit)
            message(FATAL_ERROR
                "gateway: pjproject archive for '${_spec}' not found under "
                "${PJSIP_MSVC_LIB_DIRS}. Build the pjproject tree first and pass "
                "-DPJSIP_LIB_DIR=<dir holding the archives>.")
        endif()
        list(APPEND PJSIP_LIBS "${_hit}")
    endforeach()

    foreach(_spec IN LISTS _pj_optional)
        string(REPLACE "|" ";" _names "${_spec}")
        set(_hit "")
        foreach(_name IN LISTS _names)
            foreach(_suffix IN LISTS PJSIP_MSVC_LIB_SUFFIXES)
                find_library(PJSIP_${_name}${_suffix}_LIB NAMES "${_name}${_suffix}"
                    PATHS ${PJSIP_MSVC_LIB_DIRS} NO_DEFAULT_PATH)
                if(NOT _hit AND PJSIP_${_name}${_suffix}_LIB)
                    set(_hit "${PJSIP_${_name}${_suffix}_LIB}")
                endif()
            endforeach()
        endforeach()
        if(_hit)
            list(APPEND PJSIP_LIBS "${_hit}")
        endif()
    endforeach()

    message(STATUS "gateway: pjproject archives:")
    foreach(_lib IN LISTS PJSIP_LIBS)
        message(STATUS "    ${_lib}")
    endforeach()

    target_link_libraries(gateway PUBLIC ${PJSIP_LIBS})

    # pjproject 在 Windows 上的系统依赖（Winsock / COM / 多媒体 / 安全）。
    target_link_libraries(gateway PUBLIC
        ws2_32 winmm ole32 secur32 crypt32 iphlpapi)
else()
    # POSIX（Linux 及后续 macOS）走源码构建：pjproject 自己的 Makefile 把每个
    # 模块的归档放在 <module>/lib，文件名是 lib<module>-<TARGET_NAME>.a
    # （TARGET_NAME 见 build.mak，例如 x86_64-pc-linux-gnu）。
    if(NOT PJSIP_ROOT)
        message(FATAL_ERROR "gateway: set -DPJSIP_ROOT=<pjproject tree>")
    endif()

    set(PJ_TARGET_NAME "")
    if(EXISTS "${PJSIP_ROOT}/build.mak")
        file(STRINGS "${PJSIP_ROOT}/build.mak" _pj_target_line
             REGEX "^export TARGET_NAME[ \t]*:=")
        if(_pj_target_line)
            string(REGEX REPLACE "^export TARGET_NAME[ \t]*:=[ \t]*" ""
                   PJ_TARGET_NAME "${_pj_target_line}")
            string(STRIP "${PJ_TARGET_NAME}" PJ_TARGET_NAME)
        endif()
    endif()
    if(NOT PJ_TARGET_NAME)
        message(WARNING
            "gateway: TARGET_NAME not found in ${PJSIP_ROOT}/build.mak; "
            "falling back to unsuffixed pjproject library names")
    endif()

    set(PJSIP_LIB_DIRS
        ${PJSIP_ROOT}/pjsip/lib
        ${PJSIP_ROOT}/pjmedia/lib
        ${PJSIP_ROOT}/pjnath/lib
        ${PJSIP_ROOT}/pjlib-util/lib
        ${PJSIP_ROOT}/pjlib/lib
        ${PJSIP_ROOT}/third_party/lib
        ${PJSIP_ROOT}/lib
    )
    if(PJSIP_LIB_DIR)
        list(INSERT PJSIP_LIB_DIRS 0 ${PJSIP_LIB_DIR})
    endif()

    # 与 build.mak 的 APP_LIBXX_FILES 同集合。顺序只影响可读性，
    # 静态库之间的循环引用由下面的 --start-group 兜住。
    set(PJSIP_MODULES
        pjsua2 pjsua pjsip-ua pjsip-simple pjsip
        pjmedia-codec pjmedia-videodev pjmedia pjmedia-audiodev
        pjnath pjlib-util pj
    )

    set(PJSIP_LIBS "")
    foreach(mod IN LISTS PJSIP_MODULES)
        set(_names ${mod})
        if(PJ_TARGET_NAME)
            list(INSERT _names 0 "${mod}-${PJ_TARGET_NAME}")
        endif()
        string(REPLACE "-" "_" _var "${mod}")
        find_library(PJSIP_${_var}_LIB NAMES ${_names}
                     PATHS ${PJSIP_LIB_DIRS} NO_DEFAULT_PATH)
        if(NOT PJSIP_${_var}_LIB)
            message(FATAL_ERROR
                "gateway: pjproject library '${mod}' not found under ${PJSIP_ROOT}. "
                "Build the tree first (make -j) and re-run configure.")
        endif()
        list(APPEND PJSIP_LIBS ${PJSIP_${_var}_LIB})
    endforeach()

    # 第三方归档（srtp / yuv / webrtc / speex / gsm / ilbc / g7221 ...）由
    # pjproject 自己构建在 third_party/lib，文件名不统一，按目录收集。
    # PJSIP_LIB_DIR 为空时不能把空串拼进 glob（会得到 "/lib*.a" 去扫根目录）。
    set(_pj_third_party_globs "${PJSIP_ROOT}/third_party/lib/lib*.a")
    if(PJSIP_LIB_DIR)
        list(APPEND _pj_third_party_globs "${PJSIP_LIB_DIR}/lib*.a")
    endif()
    file(GLOB PJSIP_THIRD_PARTY_LIBS ${_pj_third_party_globs})
    list(REMOVE_DUPLICATES PJSIP_THIRD_PARTY_LIBS)

    if(APPLE)
        target_link_libraries(gateway PUBLIC ${PJSIP_LIBS} ${PJSIP_THIRD_PARTY_LIBS})
    else()
        # GNU ld：静态库之间互有引用，用 group 消除链接顺序依赖。
        target_link_libraries(gateway PUBLIC
            -Wl,--start-group
            ${PJSIP_LIBS} ${PJSIP_THIRD_PARTY_LIBS}
            -Wl,--end-group
        )
    endif()

    target_link_libraries(gateway PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})

    # pjproject 在 POSIX 上找到 OpenSSL 时会启用 TLS socket。带上它，
    # 避免 pjlib 里 SSL 符号未解析。
    find_package(OpenSSL QUIET)
    if(OPENSSL_FOUND)
        target_link_libraries(gateway PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    endif()
endif()