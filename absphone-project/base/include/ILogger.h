#pragma once

#include "base_export.h"
#include "Response.h"
#include <string>
#include <memory>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace vcs { namespace base {

class IConfig;   // 前置声明：日志初始化/重载只从 IConfig 取配置

// ============================================================
// 内部辅助：格式化消息
// ============================================================

// 使用 vsnprintf 格式化消息
inline std::string FormatMessage(const char* format, va_list args) {
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    return std::string(buffer);
}

// 带可变参数的格式化（供日志宏使用）
inline std::string FormatMessageF(const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return std::string(buffer);
}

// 日志级别
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
    Off          // 追加在末尾：旧数值 0..5 不变；Off 关闭该 logger 全部输出
};

// 日志模块类型
enum class LogModule {
    Common,      // 公共模块
    Base,        // 基础模块
    Gateway,     // 网关模块
    Conference,  // 会议模块
    Media,       // 媒体模块
    AI,          // AI模块
    Max
};

// 获取模块名称
VCS_BASE_API const char* GetModuleName(LogModule module);

// 日志级别枚举到字符串的映射
VCS_BASE_API const char* LogLevelToString(LogLevel level);

// 将字符串转换为日志级别
VCS_BASE_API LogLevel StringToLogLevel(const std::string& levelStr);

// 日志配置
struct LogConfig {
    bool enabled = true;           // 是否启用
    LogLevel level = LogLevel::Info;  // 日志级别（默认输出Info及以上）
    bool toFile = true;            // 是否写入文件
    bool toConsole = true;         // 是否输出到控制台
    std::string filePath;          // 日志文件路径
    size_t maxFileSize = 10 * 1024 * 1024;  // 单文件最大大小(字节)
    int maxFiles = 5;              // 保留的日志文件数量
    int retentionDays = 0;         // 按天清理；0 = 不按天清理
};

// 日志自诊断计数（GetStats 汇总）
struct LogStats {
    uint64_t linesWritten = 0;
    uint64_t writeFailures = 0;
    uint64_t rotationFailures = 0;
    uint64_t droppedLines = 0;     // 预留：同步写当下恒为 0
};

// ============================================================
// 轻量日志上下文：线程内生效的 traceId / sessionId。
// 本期只提供机制，未铺开调用链时输出 "-"（docs/10 §3.2.5）。
// ============================================================
class VCS_BASE_API ScopedLogContext {
public:
    ScopedLogContext(const std::string& traceId, const std::string& sessionId);
    ~ScopedLogContext();

    ScopedLogContext(const ScopedLogContext&) = delete;
    ScopedLogContext& operator=(const ScopedLogContext&) = delete;

private:
    std::string prevTraceId_;
    std::string prevSessionId_;
};

// 日志接口 - base模块提供，其他模块使用
class VCS_BASE_API ILogger {
public:
    virtual ~ILogger() = default;

    // 基础方法：直接传字符串
    virtual void Trace(const std::string& message) = 0;
    virtual void Debug(const std::string& message) = 0;
    virtual void Info(const std::string& message) = 0;
    virtual void Warning(const std::string& message) = 0;
    virtual void Error(const std::string& message) = 0;
    virtual void Fatal(const std::string& message) = 0;

    // 带标签的日志
    virtual void Log(LogLevel level, const std::string& tag, const std::string& message) = 0;

    // ============================================================
    // 格式化方法：支持 printf 风格
    // ============================================================
    // 使用方式: logger->InfoF("User %s joined, count=%d", "Alice", 123);

    virtual void TraceF(const char* format, ...) = 0;
    virtual void DebugF(const char* format, ...) = 0;
    virtual void InfoF(const char* format, ...) = 0;
    virtual void WarningF(const char* format, ...) = 0;
    virtual void ErrorF(const char* format, ...) = 0;
    virtual void FatalF(const char* format, ...) = 0;

    // 带标签的格式化日志
    virtual void LogF(LogLevel level, const std::string& tag, const char* format, ...) = 0;

    // ============================================================
    // 带源码位置信息的日志方法（文件名、行号、函数名）
    // ============================================================

    // 带源码位置的格式化日志
    virtual void TraceFL(const char* file, int line, const char* function, const char* format, ...) = 0;
    virtual void DebugFL(const char* file, int line, const char* function, const char* format, ...) = 0;
    virtual void InfoFL(const char* file, int line, const char* function, const char* format, ...) = 0;
    virtual void WarningFL(const char* file, int line, const char* function, const char* format, ...) = 0;
    virtual void ErrorFL(const char* file, int line, const char* function, const char* format, ...) = 0;
    virtual void FatalFL(const char* file, int line, const char* function, const char* format, ...) = 0;

    // 带源码位置的日志（直接传字符串）
    virtual void TraceL(const char* file, int line, const char* function, const std::string& message) = 0;
    virtual void DebugL(const char* file, int line, const char* function, const std::string& message) = 0;
    virtual void InfoL(const char* file, int line, const char* function, const std::string& message) = 0;
    virtual void WarningL(const char* file, int line, const char* function, const std::string& message) = 0;
    virtual void ErrorL(const char* file, int line, const char* function, const std::string& message) = 0;
    virtual void FatalL(const char* file, int line, const char* function, const std::string& message) = 0;

    // 获取模块名
    virtual const char* GetModuleName() const = 0;

    // 设置日志级别
    virtual void SetLevel(LogLevel level) = 0;
    virtual LogLevel GetLevel() const = 0;

    // 获取/设置配置
    virtual void SetConfig(const LogConfig& config) = 0;
    virtual LogConfig GetConfig() const = 0;
};

// ============================================================
// 日志宏：简化调用
// ============================================================

// 简单日志宏（直接传字符串）
#define VCS_LOG_TRACE(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->Trace(msg)

#define VCS_LOG_DEBUG(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->Debug(msg)

#define VCS_LOG_INFO(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->Info(msg)

#define VCS_LOG_WARNING(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->Warning(msg)

#define VCS_LOG_ERROR(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->Error(msg)

#define VCS_LOG_FATAL(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->Fatal(msg)

// 格式化日志宏（printf 风格）
#define VCS_LOGF_TRACE(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->TraceF(format, ##__VA_ARGS__)

#define VCS_LOGF_DEBUG(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->DebugF(format, ##__VA_ARGS__)

#define VCS_LOGF_INFO(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->InfoF(format, ##__VA_ARGS__)

#define VCS_LOGF_WARNING(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->WarningF(format, ##__VA_ARGS__)

#define VCS_LOGF_ERROR(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->ErrorF(format, ##__VA_ARGS__)

#define VCS_LOGF_FATAL(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->FatalF(format, ##__VA_ARGS__)

// 带标签的日志宏
#define VCS_LOGT(module, level, tag, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->Log(level, tag, msg)

#define VCS_LOGTF(module, level, tag, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->LogF(level, tag, format, ##__VA_ARGS__)

// ============================================================
// 带源码位置的日志宏（自动记录文件名、行号、函数名）
// 推荐使用这些宏，方便定位问题
// ============================================================

// 带源码位置的格式化日志宏
#define VCS_LOG_TRACE_L(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->TraceFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_LOG_DEBUG_L(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->DebugFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_LOG_INFO_L(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->InfoFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_LOG_WARNING_L(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->WarningFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_LOG_ERROR_L(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->ErrorFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_LOG_FATAL_L(module, format, ...) \
    vcs::base::GetLogManager()->GetLogger(module)->FatalFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

// 带源码位置的日志宏（直接传字符串）
#define VCS_LOG_TRACE_LS(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->TraceL(__FILE__, __LINE__, __FUNCTION__, msg)

#define VCS_LOG_DEBUG_LS(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->DebugL(__FILE__, __LINE__, __FUNCTION__, msg)

#define VCS_LOG_INFO_LS(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->InfoL(__FILE__, __LINE__, __FUNCTION__, msg)

#define VCS_LOG_WARNING_LS(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->WarningL(__FILE__, __LINE__, __FUNCTION__, msg)

#define VCS_LOG_ERROR_LS(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->ErrorL(__FILE__, __LINE__, __FUNCTION__, msg)

#define VCS_LOG_FATAL_LS(module, msg) \
    vcs::base::GetLogManager()->GetLogger(module)->FatalL(__FILE__, __LINE__, __FUNCTION__, msg)

// 全局日志宏
#define VCS_GLOBAL_TRACE(msg)   vcs::base::GetLogManager()->GetGlobalLogger()->Trace(msg)
#define VCS_GLOBAL_DEBUG(msg)    vcs::base::GetLogManager()->GetGlobalLogger()->Debug(msg)
#define VCS_GLOBAL_INFO(msg)     vcs::base::GetLogManager()->GetGlobalLogger()->Info(msg)
#define VCS_GLOBAL_WARNING(msg)  vcs::base::GetLogManager()->GetGlobalLogger()->Warning(msg)
#define VCS_GLOBAL_ERROR(msg)    vcs::base::GetLogManager()->GetGlobalLogger()->Error(msg)
#define VCS_GLOBAL_FATAL(msg)    vcs::base::GetLogManager()->GetGlobalLogger()->Fatal(msg)

// 带源码位置的全剧日志宏
#define VCS_GLOBAL_TRACE_L(format, ...)   vcs::base::GetLogManager()->GetGlobalLogger()->TraceFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)
#define VCS_GLOBAL_DEBUG_L(format, ...)    vcs::base::GetLogManager()->GetGlobalLogger()->DebugFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)
#define VCS_GLOBAL_INFO_L(format, ...)    vcs::base::GetLogManager()->GetGlobalLogger()->InfoFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)
#define VCS_GLOBAL_WARNING_L(format, ...)  vcs::base::GetLogManager()->GetGlobalLogger()->WarningFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)
#define VCS_GLOBAL_ERROR_L(format, ...)    vcs::base::GetLogManager()->GetGlobalLogger()->ErrorFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)
#define VCS_GLOBAL_FATAL_L(format, ...)    vcs::base::GetLogManager()->GetGlobalLogger()->FatalFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

// ============================================================
// 便捷宏：直接使用当前模块（日志输出更简洁）
// 在 Media 模块中可以写 VCS_MEDIA_LOG_INFO("message")
// ============================================================
#define VCS_MEDIA_LOG_TRACE(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Media)->TraceFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_MEDIA_LOG_DEBUG(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Media)->DebugFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_MEDIA_LOG_INFO(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Media)->InfoFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_MEDIA_LOG_WARNING(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Media)->WarningFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_MEDIA_LOG_ERROR(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Media)->ErrorFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_MEDIA_LOG_FATAL(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Media)->FatalFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_CONFERENCE_LOG_TRACE(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Conference)->TraceFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_CONFERENCE_LOG_DEBUG(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Conference)->DebugFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_CONFERENCE_LOG_INFO(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Conference)->InfoFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_CONFERENCE_LOG_WARNING(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Conference)->WarningFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_CONFERENCE_LOG_ERROR(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Conference)->ErrorFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_CONFERENCE_LOG_FATAL(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Conference)->FatalFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_GATEWAY_LOG_TRACE(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Gateway)->TraceFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_GATEWAY_LOG_DEBUG(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Gateway)->DebugFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_GATEWAY_LOG_INFO(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Gateway)->InfoFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_GATEWAY_LOG_WARNING(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Gateway)->WarningFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_GATEWAY_LOG_ERROR(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Gateway)->ErrorFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

#define VCS_GATEWAY_LOG_FATAL(format, ...) \
    vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Gateway)->FatalFL(__FILE__, __LINE__, __FUNCTION__, format, ##__VA_ARGS__)

// 日志管理器接口 - 用于获取各模块的Logger实例
class VCS_BASE_API ILogManager {
public:
    virtual ~ILogManager() = default;

    // 获取指定模块的Logger
    virtual ILogger* GetLogger(LogModule module) = 0;

    // 获取按名字标识的Logger（新模块不必改 base）
    virtual ILogger* GetLogger(const std::string& moduleName) = 0;

    // 获取全局Logger（所有模块共享）
    virtual ILogger* GetGlobalLogger() = 0;

    // 初始化日志系统：配置唯一来源为 IConfig；
    // logDir 为空时使用 "logs"。
    virtual Response Initialize(std::shared_ptr<IConfig> config,
                                const std::string& logDir) = 0;

    // 重新从持有的 IConfig 解析全部 logger 配置（模块级运行期覆盖保留）
    virtual Response Reload() = 0;

    // 汇总自诊断计数
    virtual LogStats GetStats() const = 0;

    // 当前日志目录（Initialize 传入的 logDir，未初始化时为空）。
    // 供其他子系统（如 webrtc 媒体引擎）把独立日志落到同一目录。
    virtual std::string GetLogDirectory() const = 0;

    // 刷新所有日志缓冲区
    virtual void Flush() = 0;

    // 关闭日志系统
    virtual void Shutdown() = 0;
};

// 获取日志管理器单例
VCS_BASE_API ILogManager* GetLogManager();

}} // namespace vcs::base
