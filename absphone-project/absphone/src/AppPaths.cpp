#include "AppPaths.h"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <vector>

namespace vcs {
namespace absphone {

namespace {

std::string ExecutableDir() {
	namespace fs = boost::filesystem;
	boost::system::error_code ec;
	fs::path exe = boost::dll::program_location(ec);
	if (!ec && !exe.empty()) {
		return exe.parent_path().string();
	}
	boost::system::error_code cwdEc;
	fs::path cwd = fs::current_path(cwdEc);
	return cwdEc ? std::string(".") : cwd.string();
}

bool FileExists(const std::string& path) {
	namespace fs = boost::filesystem;
	boost::system::error_code ec;
	return fs::is_regular_file(fs::path(path), ec) && !ec;
}

} // namespace

AppPaths ResolveAppPaths() {
	namespace fs = boost::filesystem;

	AppPaths paths;
	paths.baseDir = ExecutableDir();

	std::vector<std::string> candidates;
	candidates.push_back((fs::path(paths.baseDir) / "config" / "hdmeeting.ini").string());
	candidates.push_back((fs::path(paths.baseDir) / "hdmeeting.ini").string());
	candidates.push_back("../config/hdmeeting.ini");   // 旧部署：相对启动目录

	// 首个存在者；都不存在时保留首选路径，由 LoadFromFile 走"缺失"分支（不造文件）
	paths.configFile = candidates.front();
	for (size_t i = 0; i < candidates.size(); ++i) {
		if (FileExists(candidates[i])) {
			paths.configFile = candidates[i];
			break;
		}
	}

	// 媒体调试旁路的开关独立成文件：hdmeeting.ini 的键集不变。
	paths.mediaDebugFile =
		(fs::path(paths.configFile).parent_path() / "media_debug.ini").string();

	paths.logDir = (fs::path(paths.baseDir) / "logs").string();
	return paths;
}

} // namespace absphone
} // namespace vcs
