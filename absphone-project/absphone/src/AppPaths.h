#pragma once

#include <string>

namespace vcs {
namespace absphone {

// 组合根路径解析（docs/10 §3.1.5）：base 不认识产品目录，
// 配置/日志路径由 absphone（组合根）解析后以绝对路径交给各组件。
struct AppPaths {
	std::string baseDir;      // 可执行文件所在目录
	std::string configFile;   // 首个存在的配置候选；都不存在时取首选路径（不造文件）
	std::string mediaDebugFile; // 与 hdmeeting.ini 同目录的 media_debug.ini（可缺省）
	std::string logDir;       // <baseDir>/logs
};

AppPaths ResolveAppPaths();

} // namespace absphone
} // namespace vcs
