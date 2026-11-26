#pragma once

// Unified symbol export macros (VCS = Video Conference System).
// Static libraries need no decoration; only shared libraries
// (base / absphone / codec_soft) distinguish export from import.
// The build system defines <MODULE>_EXPORTS for shared targets (docs/04 section 4).
//
// base 必须是共享库：它以 static 形式链入多个共享模块时，GetLogManager() 等
// 进程级单例会各自复制一份，日志总开关/级别只对其中一份生效（见 docs/10 §3.2.10）。

#ifdef _WIN32

    #ifdef VCS_BASE_EXPORTS
        #define VCS_BASE_API __declspec(dllexport)
    #else
        #define VCS_BASE_API __declspec(dllimport)
    #endif

    #ifdef VCS_ABSPHONE_EXPORTS
        #define VCS_ABSPHONE_API __declspec(dllexport)
    #else
        #define VCS_ABSPHONE_API __declspec(dllimport)
    #endif

    #ifdef VCS_CODEC_EXPORTS
        #define VCS_CODEC_API __declspec(dllexport)
    #else
        #define VCS_CODEC_API __declspec(dllimport)
    #endif

#else

    #define VCS_BASE_API      __attribute__((visibility("default")))
    #define VCS_ABSPHONE_API __attribute__((visibility("default")))
    #define VCS_CODEC_API     __attribute__((visibility("default")))

#endif

// Static libraries: symbols are linked into the final binary, no decoration.
#define VCS_COMMON_API
#define VCS_MEDIA_API
#define VCS_GATEWAY_API
#define VCS_CONFERENCE_API
#define VCS_AI_API
