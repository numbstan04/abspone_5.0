#pragma once

/**
 * @file StackLogConfig.h
 * @brief 协议栈（pjsip/pjmedia）日志配置
 *
 * pjsip 有自己的日志通道，默认不跟随应用日志级别。上层通过配置决定
 * 协议栈日志是否打开、开到什么程度、写到哪里：
 *   gateway.sip.pjsip_log_level    int     -1=跟随 log.level（默认），0..6 显式指定
 *   gateway.sip.pjsip_msg_logging  bool    SIP 报文转储（含鉴权头，按配置决定）
 *   gateway.sip.pjsip_log_file     string  pjsip 独立日志文件，默认 siplog.log
 *
 * pjsip 日志只写入自己的文件（追加），不再进入 Gateway.log。
 *
 * pj_log 级别：0=off, 1=fatal, 2=error, 3=warning, 4=info, 5=debug, 6=trace
 */

#include <algorithm>
#include <string>

namespace vcs {
namespace gateway {

struct PjsipLogSettings {
    /// pj_log 级别 0..6
    int level = 4;
    /// 是否转储 SIP 报文（Authorization 等敏感头会进入日志）
    bool msgLogging = false;
    /// pjsip 独立日志文件（追加写）。空串表示不落文件。
    std::string logFile = "siplog.log";
};

/// 把任意值夹到 pj_log 的合法区间
inline int ClampPjsipLogLevel(int level) {
    return std::max(0, std::min(6, level));
}

/**
 * @brief 应用日志级别（vcs::base::LogLevel 的数值）→ pj_log 级别
 *
 * Trace(0)→6, Debug(1)→5, Info(2)→4, Warning(3)→3, Error(4)→2, Fatal(5)→1, Off(6)→0
 */
inline int MapAppLogLevelToPjsip(int appLevel) {
    return ClampPjsipLogLevel(6 - appLevel);
}

} // namespace gateway
} // namespace vcs
