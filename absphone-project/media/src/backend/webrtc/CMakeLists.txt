# WebRTC 后端（标准产物专用，仅 VCS_WITH_WEBRTC=ON 时由 media/CMakeLists.txt 引入）。
#
# 目标独立成库的原因：libwebrtc 对 C++ 标准与编译选项有自己的要求，
# 与 media 其余部分（C++14）解耦后，后续升级 libwebrtc 版本只改本目录。
#
# 两层结构：
#   1. WebRtcRtpBackend + IWebRtcEngine.h（版本无关中间层）—— 始终编译；
#   2. engine/LibWebRtcEngine.cpp（唯一包含 libwebrtc 头文件的适配层）——
#      由 VCS_WEBRTC_BUILD_LIBWEBRTC 控制，需要匹配的 libwebrtc 工具链。
# 未开启时使用 engine/WebRtcEngineStub.cpp，CreateWebRtcEngine() 返回 nullptr。

# libwebrtc 源码根目录。兼容三种布局：
#   <root>/windows/src  （E:\5.0\third_party\webrtc）
#   <root>/src
#   <root> 本身即 src    （E:\5.0\third_party\webrtc_m140\webrtc_m140_win\src）
set(VCS_WEBRTC_ROOT "E:/5.0/third_party/webrtc_m140/webrtc_m140_win/src"
    CACHE PATH "libwebrtc source root (contains windows/src, src, or api/)")

if(EXISTS "${VCS_WEBRTC_ROOT}/windows/src")
    set(VCS_WEBRTC_INCLUDE_DIR "${VCS_WEBRTC_ROOT}/windows/src")
elseif(EXISTS "${VCS_WEBRTC_ROOT}/src")
    set(VCS_WEBRTC_INCLUDE_DIR "${VCS_WEBRTC_ROOT}/src")
elseif(EXISTS "${VCS_WEBRTC_ROOT}/api/peer_connection_interface.h")
    set(VCS_WEBRTC_INCLUDE_DIR "${VCS_WEBRTC_ROOT}")
else()
    message(FATAL_ERROR
        "VCS_WITH_WEBRTC=ON requires the libwebrtc source tree. "
        "Set -DVCS_WEBRTC_ROOT=<path containing windows/src, src, or api/> "
        "(current: ${VCS_WEBRTC_ROOT}).")
endif()

# 是否编译真正调用 libwebrtc 的适配层。m140 需要 C++20 与匹配的编译工具链；
# 默认关闭，避免 VS2017 等旧工具链在 VCS_WITH_WEBRTC=ON 时直接编译失败。
option(VCS_WEBRTC_BUILD_LIBWEBRTC
    "Build engine/LibWebRtcEngine.cpp against the libwebrtc tree" OFF)

set(VCS_WEBRTC_SOURCES
    WebRtcRtpBackend.cpp
    WebRtcRtpBackend.h
    WebRtcLogBridge.cpp
    WebRtcLogBridge.h
)

if(VCS_WEBRTC_BUILD_LIBWEBRTC)
    # LibWebRtcEngine.cpp 仍由 MSVC 编译；LibWebRtcRtpCodec.cpp 需要匹配
    # m140 的 libc++ ABI，单独用 clang-cl 编译（见下方 custom command）。
    list(APPEND VCS_WEBRTC_SOURCES engine/LibWebRtcEngine.cpp)
else()
    list(APPEND VCS_WEBRTC_SOURCES
        engine/WebRtcEngineStub.cpp
        engine/WebRtcRtpCodecStub.cpp
        engine/WebRtcMediaEngineStub.cpp)
endif()

add_library(media_backend_webrtc STATIC ${VCS_WEBRTC_SOURCES})

target_include_directories(media_backend_webrtc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_SOURCE_DIR}/media/include
        ${CMAKE_SOURCE_DIR}/media/src
        ${CMAKE_SOURCE_DIR}/base/include
        ${CMAKE_SOURCE_DIR}/common/include
)

# 与 media 目标保持同一开关口径；中间层头文件的 #ifdef 依赖它。
target_compile_definitions(media_backend_webrtc PUBLIC VCS_WITH_WEBRTC=1)

if(VCS_WEBRTC_BUILD_LIBWEBRTC)
    # ---- libwebrtc 适配层构建参数 ------------------------------------
    # libwebrtc 生成的头文件（webrtc_buildflags.h 等）默认位于 gn out 目录。
    set(VCS_WEBRTC_GEN_DIR "${VCS_WEBRTC_INCLUDE_DIR}/out/Debug32/gen"
        CACHE PATH "libwebrtc generated headers directory")
    set(VCS_WEBRTC_LIB_DIR "${VCS_WEBRTC_INCLUDE_DIR}/out/Debug32/obj"
        CACHE PATH "libwebrtc static library directory (GN obj dir)")
    # GN 输出的是 LLVM thin archive，MSVC link.exe 无法读取；
    # 自动展开后的普通 .lib 放在这里（可复用、可增量）。
    set(VCS_WEBRTC_THICK_LIB_DIR "E:/5.0/deps/webrtc_m140_thick"
        CACHE PATH "MSVC-ready (thick) libwebrtc archives")

    set(_webrtc_archive_dir "${VCS_WEBRTC_LIB_DIR}")
    set(_webrtc_probe
        "${_webrtc_archive_dir}/api/adaptation/resource_adaptation_api.lib")
    if(EXISTS "${_webrtc_probe}")
        file(READ "${_webrtc_probe}" _webrtc_magic LIMIT 8)
        if(_webrtc_magic STREQUAL "!<thin>\n")
            if(NOT WIN32 OR NOT CMAKE_AR)
                message(FATAL_ERROR
                    "LLVM thin archives require MSVC lib.exe for conversion. "
                    "Set -DVCS_WEBRTC_THICK_LIB_DIR to a pre-converted directory.")
            endif()
            message(STATUS
                "WebRTC: converting thin archives -> ${VCS_WEBRTC_THICK_LIB_DIR}")
            execute_process(
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/engine/tools/thin_to_thick.ps1"
                    -ObjRoot "${_webrtc_archive_dir}"
                    -OutRoot "${VCS_WEBRTC_THICK_LIB_DIR}"
                    -LibExe "${CMAKE_AR}"
                RESULT_VARIABLE _webrtc_convert_result
                OUTPUT_VARIABLE _webrtc_convert_out
                ERROR_VARIABLE _webrtc_convert_err)
            if(NOT _webrtc_convert_result EQUAL 0)
                message(FATAL_ERROR
                    "thin archive conversion failed: ${_webrtc_convert_err}")
            endif()
            set(_webrtc_archive_dir "${VCS_WEBRTC_THICK_LIB_DIR}")
        endif()
    endif()

    target_include_directories(media_backend_webrtc
        PRIVATE
            ${VCS_WEBRTC_INCLUDE_DIR}
            ${VCS_WEBRTC_GEN_DIR}
            ${VCS_WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp
            ${VCS_WEBRTC_INCLUDE_DIR}/third_party/libyuv/include
            ${VCS_WEBRTC_INCLUDE_DIR}/third_party/jsoncpp/source/include
            ${VCS_WEBRTC_INCLUDE_DIR}/third_party/perfetto/include
            ${VCS_WEBRTC_INCLUDE_DIR}/third_party/boringssl/src/include
    )
    target_compile_definitions(media_backend_webrtc PRIVATE
        WEBRTC_WIN
        WEBRTC_ENABLE_PROTOBUF=0
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )

    # m140 的公共头文件使用 C++17/20 特性。
    set_target_properties(media_backend_webrtc PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
    )

    # GN 把 libwebrtc 拆成大量静态库；C++ API 没有单一 webrtc.lib。
    # 优先使用显式列表（可照搬 GN 下 peerconnection_client 的链接集合），
    # 为空时退化为收集 VCS_WEBRTC_LIB_DIR 下的全部 .lib。
    set(VCS_WEBRTC_LIBS "" CACHE STRING
        "Explicit libwebrtc archives; empty = all .lib under VCS_WEBRTC_LIB_DIR")

    set(_webrtc_link_libs ${VCS_WEBRTC_LIBS})
    if(NOT _webrtc_link_libs)
        if(EXISTS "${_webrtc_archive_dir}")
            file(GLOB_RECURSE _webrtc_link_libs
                "${_webrtc_archive_dir}/*.lib")
        endif()
    endif()

    if(_webrtc_link_libs)
        # m140 的 Debug32 使用静态 CRT（/MTd），工程其余部分使用 /MDd。
        # 屏蔽静态 CRT 的默认库，统一由工程的 msvcrtd/msvcpd 提供符号。
        target_link_options(media_backend_webrtc INTERFACE
            /NODEFAULTLIB:libcmtd.lib
            /NODEFAULTLIB:libcpmtd.lib
            # Chromium libc++ 与 MSVC STL 都定义了 std::nothrow 等少数全局
            # 常量；两份定义内容一致，取其一即可。
            /FORCE:MULTIPLE
        )

        # GN 的部分依赖是 source_set（abseil、libc++），只产出散装 .obj，
        # 没有 .lib。MSVC 链接器需要归档（或逐个对象文件），这里用 lib.exe
        # 把它们打包到 VCS_WEBRTC_THICK_LIB_DIR，保持最终链接命令行可控。
        function(vcs_webrtc_pack_obj_tree obj_tree out_name)
            set(_obj_dir "${VCS_WEBRTC_LIB_DIR}/${obj_tree}")
            set(_out_lib "${VCS_WEBRTC_THICK_LIB_DIR}/${out_name}.lib")
            if(NOT EXISTS "${_obj_dir}")
                return()
            endif()
            file(GLOB_RECURSE _pack_objs "${_obj_dir}/*.obj")
            if(NOT _pack_objs)
                return()
            endif()

            set(_need_pack TRUE)
            if(EXISTS "${_out_lib}")
                set(_need_pack FALSE)
                foreach(_o IN LISTS _pack_objs)
                    if(_o IS_NEWER_THAN "${_out_lib}")
                        set(_need_pack TRUE)
                        break()
                    endif()
                endforeach()
            endif()

            if(_need_pack)
                file(MAKE_DIRECTORY "${VCS_WEBRTC_THICK_LIB_DIR}")
                list(LENGTH _pack_objs _pack_count)
                set(_rsp "${_out_lib}.rsp")
                set(_rsp_text "")
                foreach(_o IN LISTS _pack_objs)
                    string(REPLACE "/" "\\" _ow "${_o}")
                    string(APPEND _rsp_text "\"${_ow}\"\n")
                endforeach()
                file(WRITE "${_rsp}" "${_rsp_text}")
                message(STATUS
                    "WebRTC: packing ${_pack_count} objects -> ${_out_lib}")
                execute_process(
                    COMMAND "${CMAKE_AR}" /NOLOGO
                        "/OUT:${_out_lib}" "@${_rsp}"
                    RESULT_VARIABLE _pack_rc
                    OUTPUT_VARIABLE _pack_out
                    ERROR_VARIABLE _pack_err)
                file(REMOVE "${_rsp}")
                if(NOT _pack_rc EQUAL 0)
                    message(FATAL_ERROR
                        "packing ${obj_tree} failed: ${_pack_err}")
                endif()
            endif()
            set(${out_name}_LIB "${_out_lib}" PARENT_SCOPE)
        endfunction()

        vcs_webrtc_pack_obj_tree("buildtools/third_party/libc++/libc++" libcxx_objs)
        vcs_webrtc_pack_obj_tree("third_party/abseil-cpp" absl_objs)
        # Opus 的 SSE4.1 内核是 GN source_set（只产出散装 .obj），音频引擎
        # 启用后 opus.lib 会引用这些符号，需要一并打包。
        vcs_webrtc_pack_obj_tree("third_party/opus/opus_sse41" opus_sse41_objs)
        # rnnoise 的权重表同样是 source_set，rnn_vad.lib 只含算法对象。
        vcs_webrtc_pack_obj_tree("third_party/rnnoise/rnn_vad" rnnoise_objs)
        if(libcxx_objs_LIB)
            list(APPEND _webrtc_link_libs "${libcxx_objs_LIB}")
        endif()
        if(absl_objs_LIB)
            list(APPEND _webrtc_link_libs "${absl_objs_LIB}")
        endif()
        if(opus_sse41_objs_LIB)
            list(APPEND _webrtc_link_libs "${opus_sse41_objs_LIB}")
        endif()
        if(rnnoise_objs_LIB)
            list(APPEND _webrtc_link_libs "${rnnoise_objs_LIB}")
        endif()

        # ---- LibWebRtcRtpCodec.cpp：clang-cl + Chromium libc++ -------------
        # 该文件直接调用 m140 的 RTP packetizer/depacketizer。m140 由
        # Chromium clang-cl 生成，std 符号位于 std::__Cr；用 MSVC 编译会得到
        # std:: 版本，链接必然失败。因此这里用同一套 clang-cl + libc++ 头文件
        # 单独编译成 .obj，再作为外部对象加入静态库。
        set(_webrtc_clang_cl
            "${VCS_WEBRTC_INCLUDE_DIR}/third_party/llvm-build/Release+Asserts/bin/clang-cl.exe")
        if(EXISTS "${_webrtc_clang_cl}")
            set(_rtp_codec_src
                "${CMAKE_CURRENT_SOURCE_DIR}/engine/LibWebRtcRtpCodec.cpp")
            set(_rtp_codec_obj
                "${CMAKE_CURRENT_BINARY_DIR}/clang/LibWebRtcRtpCodec.obj")
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clang")

            add_custom_command(
                OUTPUT "${_rtp_codec_obj}"
                COMMAND "${_webrtc_clang_cl}"
                    /nologo /c /std:c++20 /MTd /Od /Z7 /EHsc /Gy /FS /bigobj
                    /utf-8 /Zc:twoPhase /Brepro -m32 -msse3
                    /D_DEBUG /DDYNAMIC_ANNOTATIONS_ENABLED=1
                    /DVCS_WITH_WEBRTC=1
                    /DWEBRTC_WIN /DWEBRTC_ENABLE_PROTOBUF=0 /DWEBRTC_LIBRARY_IMPL
                    /DCHROMIUM /DABSL_ALLOCATOR_NOTHROW=1
                    /D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE
                    /D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS
                    /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_UNICODE /DUNICODE
                    /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
                    /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS
                    /D_CRT_NONSTDC_NO_WARNINGS
                    /I${VCS_WEBRTC_INCLUDE_DIR}
                    /I${VCS_WEBRTC_GEN_DIR}
                    /I${VCS_WEBRTC_INCLUDE_DIR}/buildtools/third_party/libc++
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/libc++/src/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/libyuv/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/jsoncpp/source/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/boringssl/src/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/perfetto/include
                    /I${CMAKE_CURRENT_SOURCE_DIR}
                    /I${CMAKE_CURRENT_SOURCE_DIR}/..
                    /I${CMAKE_SOURCE_DIR}/media/include
                    /I${CMAKE_SOURCE_DIR}/media/src
                    /I${CMAKE_SOURCE_DIR}/base/include
                    /I${CMAKE_SOURCE_DIR}/common/include
                    /Fo${_rtp_codec_obj}
                    ${_rtp_codec_src}
                DEPENDS
                    ${_rtp_codec_src}
                    "${CMAKE_SOURCE_DIR}/media/include/webrtc/IWebRtcRtpCodec.h"
                COMMENT "Compiling LibWebRtcRtpCodec.cpp with clang-cl (libc++ ABI)"
                VERBATIM)

            set_source_files_properties("${_rtp_codec_obj}" PROPERTIES
                EXTERNAL_OBJECT TRUE
                GENERATED TRUE)
            target_sources(media_backend_webrtc PRIVATE "${_rtp_codec_obj}")

            # ---- LibWebRtcMediaEngine.cpp：Call / pacer / NACK / AEC --------
            # 与 RTP codec 相同，必须用 clang-cl + Chromium libc++ 编译。
            set(_media_engine_src
                "${CMAKE_CURRENT_SOURCE_DIR}/engine/LibWebRtcMediaEngine.cpp")
            set(_media_engine_obj
                "${CMAKE_CURRENT_BINARY_DIR}/clang/LibWebRtcMediaEngine.obj")

            add_custom_command(
                OUTPUT "${_media_engine_obj}"
                COMMAND "${_webrtc_clang_cl}"
                    /nologo /c /std:c++20 /MTd /Od /Z7 /EHsc /Gy /FS /bigobj
                    /utf-8 /Zc:twoPhase /Brepro -m32 -msse3
                    /D_DEBUG /DDYNAMIC_ANNOTATIONS_ENABLED=1
                    /DVCS_WITH_WEBRTC=1
                    /DWEBRTC_WIN /DWEBRTC_ENABLE_PROTOBUF=0 /DWEBRTC_LIBRARY_IMPL
                    /DCHROMIUM /DABSL_ALLOCATOR_NOTHROW=1
                    /D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE
                    /D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS
                    /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_UNICODE /DUNICODE
                    /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
                    /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS
                    /D_CRT_NONSTDC_NO_WARNINGS
                    /I${VCS_WEBRTC_INCLUDE_DIR}
                    /I${VCS_WEBRTC_GEN_DIR}
                    /I${VCS_WEBRTC_INCLUDE_DIR}/buildtools/third_party/libc++
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/libc++/src/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/libyuv/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/jsoncpp/source/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/boringssl/src/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/perfetto/include
                    /I${CMAKE_CURRENT_SOURCE_DIR}
                    /I${CMAKE_CURRENT_SOURCE_DIR}/..
                    /I${CMAKE_SOURCE_DIR}/media/include
                    /I${CMAKE_SOURCE_DIR}/media/src
                    /I${CMAKE_SOURCE_DIR}/base/include
                    /I${CMAKE_SOURCE_DIR}/common/include
                    /Fo${_media_engine_obj}
                    ${_media_engine_src}
                DEPENDS
                    ${_media_engine_src}
                    "${CMAKE_SOURCE_DIR}/media/include/webrtc/IWebRtcMediaEngine.h"
                COMMENT "Compiling LibWebRtcMediaEngine.cpp with clang-cl (libc++ ABI)"
                VERBATIM)

            set_source_files_properties("${_media_engine_obj}" PROPERTIES
                EXTERNAL_OBJECT TRUE
                GENERATED TRUE)
            target_sources(media_backend_webrtc PRIVATE "${_media_engine_obj}")

            # ---- LibWebRtcLogSink.cpp：webrtc::LogSink（std::string） -------
            set(_webrtc_log_src
                "${CMAKE_CURRENT_SOURCE_DIR}/engine/LibWebRtcLogSink.cpp")
            set(_webrtc_log_obj
                "${CMAKE_CURRENT_BINARY_DIR}/clang/LibWebRtcLogSink.obj")

            add_custom_command(
                OUTPUT "${_webrtc_log_obj}"
                COMMAND "${_webrtc_clang_cl}"
                    /nologo /c /std:c++20 /MTd /Od /Z7 /EHsc /Gy /FS /bigobj
                    /utf-8 /Zc:twoPhase /Brepro -m32 -msse3
                    /D_DEBUG /DDYNAMIC_ANNOTATIONS_ENABLED=1
                    /DVCS_WITH_WEBRTC=1
                    /DWEBRTC_WIN /DWEBRTC_ENABLE_PROTOBUF=0 /DWEBRTC_LIBRARY_IMPL
                    /DCHROMIUM /DABSL_ALLOCATOR_NOTHROW=1
                    /D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE
                    /D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS
                    /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_UNICODE /DUNICODE
                    /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
                    /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS
                    /D_CRT_NONSTDC_NO_WARNINGS
                    /I${VCS_WEBRTC_INCLUDE_DIR}
                    /I${VCS_WEBRTC_GEN_DIR}
                    /I${VCS_WEBRTC_INCLUDE_DIR}/buildtools/third_party/libc++
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/libc++/src/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/libyuv/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/jsoncpp/source/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/boringssl/src/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/perfetto/include
                    /I${CMAKE_SOURCE_DIR}/media/include
                    /Fo${_webrtc_log_obj}
                    ${_webrtc_log_src}
                DEPENDS
                    ${_webrtc_log_src}
                    "${CMAKE_SOURCE_DIR}/media/include/webrtc/IWebRtcLog.h"
                COMMENT "Compiling LibWebRtcLogSink.cpp with clang-cl (libc++ ABI)"
                VERBATIM)

            set_source_files_properties("${_webrtc_log_obj}" PROPERTIES
                EXTERNAL_OBJECT TRUE
                GENERATED TRUE)
            target_sources(media_backend_webrtc PRIVATE "${_webrtc_log_obj}")

            # ---- builtin_audio_processing_builder.cc -------------------------
            # 该 GN 目标在当前 libwebrtc 产物里没有被任何已构建 target 依赖，
            # 因此没有 .lib / .obj；AudioProcessing（AEC3）必须通过它创建。
            # 用与上面相同的 clang-cl / libc++ 参数单独编译。
            set(_apm_builder_src
                "${VCS_WEBRTC_INCLUDE_DIR}/api/audio/builtin_audio_processing_builder.cc")
            set(_apm_builder_obj
                "${CMAKE_CURRENT_BINARY_DIR}/clang/BuiltinAudioProcessingBuilder.obj")

            add_custom_command(
                OUTPUT "${_apm_builder_obj}"
                COMMAND "${_webrtc_clang_cl}"
                    /nologo /c /std:c++20 /MTd /Od /Z7 /EHsc /Gy /FS /bigobj
                    /utf-8 /Zc:twoPhase /Brepro -m32 -msse3
                    /D_DEBUG /DDYNAMIC_ANNOTATIONS_ENABLED=1
                    /DVCS_WITH_WEBRTC=1
                    /DWEBRTC_WIN /DWEBRTC_ENABLE_PROTOBUF=0 /DWEBRTC_LIBRARY_IMPL
                    /DWEBRTC_APM_DEBUG_DUMP=0
                    /DCHROMIUM /DABSL_ALLOCATOR_NOTHROW=1
                    /D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE
                    /D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS
                    /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_UNICODE /DUNICODE
                    /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
                    /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS
                    /D_CRT_NONSTDC_NO_WARNINGS
                    /I${VCS_WEBRTC_INCLUDE_DIR}
                    /I${VCS_WEBRTC_GEN_DIR}
                    /I${VCS_WEBRTC_INCLUDE_DIR}/buildtools/third_party/libc++
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/libc++/src/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/libyuv/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/jsoncpp/source/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/boringssl/src/include
                    /I${VCS_WEBRTC_INCLUDE_DIR}/third_party/perfetto/include
                    /Fo${_apm_builder_obj}
                    ${_apm_builder_src}
                DEPENDS ${_apm_builder_src}
                COMMENT "Compiling builtin_audio_processing_builder.cc with clang-cl"
                VERBATIM)

            set_source_files_properties("${_apm_builder_obj}" PROPERTIES
                EXTERNAL_OBJECT TRUE
                GENERATED TRUE)
            target_sources(media_backend_webrtc PRIVATE "${_apm_builder_obj}")
        else()
            message(WARNING
                "clang-cl not found at ${_webrtc_clang_cl}; "
                "LibWebRtcRtpCodec.cpp is not compiled and the WebRTC RTP "
                "path will fall back to the native implementation.")
        endif()

        # clang-cl 生成的 libwebrtc 目标可能引用 compiler-rt 内建函数。
        set(_webrtc_builtins
            "${VCS_WEBRTC_INCLUDE_DIR}/third_party/llvm-build/Release+Asserts/lib/clang/21/lib/windows/clang_rt.builtins-i386.lib")
        if(EXISTS "${_webrtc_builtins}")
            list(APPEND _webrtc_link_libs "${_webrtc_builtins}")
        endif()

        target_link_libraries(media_backend_webrtc PRIVATE
            ${_webrtc_link_libs}
            winmm ws2_32 secur32 crypt32 iphlpapi
            d3d11 dxgi mfplat mfuuid shlwapi ole32
            dmoguids wmcodecdspuuid strmiids
            advapi32 comdlg32 dbghelp dnsapi gdi32 msimg32 oleaut32
            shell32 user32 usp10 uuid version wininet winspool delayimp
            amstrmid msdmo
        )
    else()
        message(WARNING
            "VCS_WEBRTC_BUILD_LIBWEBRTC=ON but no libwebrtc archive found. "
            "Build libwebrtc first, then set -DVCS_WEBRTC_LIBS=<list> or "
            "-DVCS_WEBRTC_LIB_DIR=<gn obj dir> (current: ${VCS_WEBRTC_LIB_DIR}).")
    endif()

    message(STATUS
        "WebRTC adapter: ${VCS_WEBRTC_INCLUDE_DIR} (C++20, libs: ${VCS_WEBRTC_LIB_DIR})")
else()
    # 中间层与 WebRtcRtpBackend 不依赖 libwebrtc 头文件，沿用工程基线 C++14。
    set_target_properties(media_backend_webrtc PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED YES
    )
    message(STATUS
        "WebRTC adapter: stub (set -DVCS_WEBRTC_BUILD_LIBWEBRTC=ON to link libwebrtc)")
endif()


