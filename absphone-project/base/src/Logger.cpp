#include "base_export.h"
#include "ILogger.h"
#include "IConfig.h"
#include "PlatformTime.h"
#include <fstream>
#include <map>
#include <vector>
#include <mutex>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <thread>
#include <functional>
#include <cstdint>

// Use Boost.Filesystem for cross-platform directory operations
#include <boost/filesystem.hpp>

namespace vcs {
	namespace base {

			// ============================================================
			// Helper functions
			// ============================================================

			const char* GetModuleName(LogModule module) {
				switch (module) {
				case LogModule::Common:     return "Common";
				case LogModule::Base:       return "Base";
				case LogModule::Gateway:    return "Gateway";
				case LogModule::Conference: return "Conference";
				case LogModule::Media:      return "Media";
				case LogModule::AI:         return "AI";
				default:                    return "Unknown";
				}
			}

			const char* LogLevelToString(LogLevel level) {
				switch (level) {
				case LogLevel::Trace:   return "TRACE";
				case LogLevel::Debug:   return "DEBUG";
				case LogLevel::Info:    return "INFO ";
				case LogLevel::Warning: return "WARN ";
				case LogLevel::Error:   return "ERROR";
				case LogLevel::Fatal:   return "FATAL";
				case LogLevel::Off:     return "OFF  ";
				default: return "UNK  ";
				}
			}

			LogLevel StringToLogLevel(const std::string& levelStr) {
				std::string lower = levelStr;
				std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

				// docs/10 §3.2.3：0 Trace … 5 Fatal，6 Off
				if (lower == "0") return LogLevel::Trace;
				if (lower == "1") return LogLevel::Debug;
				if (lower == "2") return LogLevel::Info;
				if (lower == "3") return LogLevel::Warning;
				if (lower == "4") return LogLevel::Error;
				if (lower == "5") return LogLevel::Fatal;
				if (lower == "6") return LogLevel::Off;

				if (lower == "trace") return LogLevel::Trace;
				if (lower == "debug") return LogLevel::Debug;
				if (lower == "info")  return LogLevel::Info;
				if (lower == "warning" || lower == "warn") return LogLevel::Warning;
				if (lower == "error") return LogLevel::Error;
				if (lower == "fatal") return LogLevel::Fatal;
				if (lower == "off" || lower == "none") return LogLevel::Off;
				return LogLevel::Info;
			}

			// Get timestamp string
			// 线程内日志上下文（ScopedLogContext 维护，docs/10 §3.2.5）
			static thread_local std::string t_traceId;
			static thread_local std::string t_sessionId;

			static const std::string& LogTraceId() { return t_traceId; }
			static const std::string& LogSessionId() { return t_sessionId; }

			static uint64_t CurrentThreadId() {
				return static_cast<uint64_t>(
					std::hash<std::thread::id>()(std::this_thread::get_id()));
			}

			// Ensure directory exists
			bool EnsureDirectory(const std::string& path) {
				namespace fs = boost::filesystem;
				boost::system::error_code ec;
				fs::path dirPath(path);
				if (fs::exists(dirPath, ec)) {
					return fs::is_directory(dirPath, ec);
				}
				fs::create_directories(dirPath, ec);
				return !ec && fs::is_directory(dirPath, ec);
			}

			static std::string LowerAscii(const std::string& s) {
				std::string out = s;
				for (size_t i = 0; i < out.size(); ++i) {
					out[i] = static_cast<char>(::tolower(static_cast<unsigned char>(out[i])));
				}
				return out;
			}

			// "<stem>.<n>.log" 判定：只认纯数字份号，避免误伤别的模块/别的文件。
			static bool IsArchiveOf(const std::string& fileName, const std::string& stem) {
				const std::string prefix = stem + ".";
				const std::string suffix = ".log";
				if (fileName.size() <= prefix.size() + suffix.size()) return false;
				if (fileName.compare(0, prefix.size(), prefix) != 0) return false;
				if (fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) != 0)
					return false;
				const std::string digits = fileName.substr(
					prefix.size(), fileName.size() - prefix.size() - suffix.size());
				if (digits.empty()) return false;
				for (size_t i = 0; i < digits.size(); ++i) {
					if (digits[i] < '0' || digits[i] > '9') return false;
				}
				return true;
			}

			ScopedLogContext::ScopedLogContext(const std::string& traceId,
											   const std::string& sessionId)
				: prevTraceId_(t_traceId), prevSessionId_(t_sessionId) {
				t_traceId = traceId;
				t_sessionId = sessionId;
			}

			ScopedLogContext::~ScopedLogContext() {
				t_traceId = prevTraceId_;
				t_sessionId = prevSessionId_;
			}

			// ============================================================
			// 配置解析（唯一来源 IConfig，docs/10 §3.2.2）
			// ============================================================

			struct LogSettings {
				bool enabled = true;
				LogLevel level = LogLevel::Info;
				bool toFile = true;
				bool toConsole = true;
				size_t maxFileSize = 10 * 1024 * 1024;
				int maxFiles = 5;
				int retentionDays = 0;
			};

			// 规范键优先，旧文件裸键兜底（未注入别名表的宿主也能读到旧键）。
			// 只读 IConfig：不得持有 LogManager::mutex_ 调用（IConfig 内部可能打日志）。
			static LogSettings ReadLogSettings(const std::shared_ptr<IConfig>& config,
											   std::vector<std::string>* warnings) {
				LogSettings s;
				if (!config) return s;

				s.enabled = config->GetBool("log.enabled", config->GetBool("EnableLog", true));

				const int defaultLevel = static_cast<int>(LogLevel::Info);
				// 三种拼写都接受：规范长键 / 规范短键（SetLogLevel 的落点）/ 旧裸键
				int level = config->GetInt("log.level",
					config->GetInt("log_level", config->GetInt("LogLevel", defaultLevel)));
				if (level < 0 || level > static_cast<int>(LogLevel::Off)) {
					if (warnings) {
						std::ostringstream oss;
						oss << "invalid log level " << level << " (expected 0..6), using "
						    << defaultLevel;
						warnings->push_back(oss.str());
					}
					level = defaultLevel;
				}
				s.level = static_cast<LogLevel>(level);

				s.toFile = config->GetBool("log.file", true);
				s.toConsole = config->GetBool("log.console", true);

				int sizeMb = config->GetInt("log.size_mb",
					config->GetInt("log_size", config->GetInt("LogSize", 10)));
				if (sizeMb <= 0) {
					if (warnings) {
						std::ostringstream oss;
						oss << "invalid log size " << sizeMb << " MB (expected > 0), using 10";
						warnings->push_back(oss.str());
					}
					sizeMb = 10;
				}
				s.maxFileSize = static_cast<size_t>(sizeMb) * 1024 * 1024;

				int maxFiles = config->GetInt("log.max_files", 5);
				if (maxFiles < 1) {
					if (warnings) {
						std::ostringstream oss;
						oss << "invalid log max_files " << maxFiles << " (expected >= 1), using 5";
						warnings->push_back(oss.str());
					}
					maxFiles = 5;
				}
				s.maxFiles = maxFiles;

				int retention = config->GetInt("log.retention_days", 0);
				if (retention < 0) {
					if (warnings) {
						std::ostringstream oss;
						oss << "invalid log retention_days " << retention << " (expected >= 0), using 0";
						warnings->push_back(oss.str());
					}
					retention = 0;
				}
				s.retentionDays = retention;
				return s;
			}

			// ============================================================
			// ModuleLogger Implementation
			// ============================================================

			class ModuleLogger : public ILogger {
			public:
				ModuleLogger(const std::string& name, const LogConfig& config)
					: name_(name), config_(config) {
					if (config_.filePath.empty()) {
						config_.filePath = "logs/" + name_ + ".log";
					}
				}

				const char* GetModuleName() const override {
					return name_.c_str();
				}

				const std::string& Name() const { return name_; }

				void Trace(const std::string& message) override { Log(LogLevel::Trace, "", message); }
				void Debug(const std::string& message) override { Log(LogLevel::Debug, "", message); }
				void Info(const std::string& message) override { Log(LogLevel::Info, "", message); }
				void Warning(const std::string& message) override { Log(LogLevel::Warning, "", message); }
				void Error(const std::string& message) override { Log(LogLevel::Error, "", message); }
				void Fatal(const std::string& message) override { Log(LogLevel::Fatal, "", message); }

				// Format method implementations
				void TraceF(const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					Log(LogLevel::Trace, "", msg);
				}

				void DebugF(const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					Log(LogLevel::Debug, "", msg);
				}

				void InfoF(const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					Log(LogLevel::Info, "", msg);
				}

				void WarningF(const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					Log(LogLevel::Warning, "", msg);
				}

				void ErrorF(const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					Log(LogLevel::Error, "", msg);
				}

				void FatalF(const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					Log(LogLevel::Fatal, "", msg);
				}

				void LogF(LogLevel level, const std::string& tag, const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					Log(level, tag, msg);
				}

				// Log methods with source location
				void TraceFL(const char* file, int line, const char* function, const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					LogWithLocation(LogLevel::Trace, file, line, function, "", msg);
				}

				void DebugFL(const char* file, int line, const char* function, const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					LogWithLocation(LogLevel::Debug, file, line, function, "", msg);
				}

				void InfoFL(const char* file, int line, const char* function, const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					LogWithLocation(LogLevel::Info, file, line, function, "", msg);
				}

				void WarningFL(const char* file, int line, const char* function, const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					LogWithLocation(LogLevel::Warning, file, line, function, "", msg);
				}

				void ErrorFL(const char* file, int line, const char* function, const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					LogWithLocation(LogLevel::Error, file, line, function, "", msg);
				}

				void FatalFL(const char* file, int line, const char* function, const char* format, ...) override {
					va_list args;
					va_start(args, format);
					std::string msg = FormatMessage(format, args);
					va_end(args);
					LogWithLocation(LogLevel::Fatal, file, line, function, "", msg);
				}

				// Log with source location (string message)
				void TraceL(const char* file, int line, const char* function, const std::string& message) override {
					LogWithLocation(LogLevel::Trace, file, line, function, "", message);
				}

				void DebugL(const char* file, int line, const char* function, const std::string& message) override {
					LogWithLocation(LogLevel::Debug, file, line, function, "", message);
				}

				void InfoL(const char* file, int line, const char* function, const std::string& message) override {
					LogWithLocation(LogLevel::Info, file, line, function, "", message);
				}

				void WarningL(const char* file, int line, const char* function, const std::string& message) override {
					LogWithLocation(LogLevel::Warning, file, line, function, "", message);
				}

				void ErrorL(const char* file, int line, const char* function, const std::string& message) override {
					LogWithLocation(LogLevel::Error, file, line, function, "", message);
				}

				void FatalL(const char* file, int line, const char* function, const std::string& message) override {
					LogWithLocation(LogLevel::Fatal, file, line, function, "", message);
				}

				void Log(LogLevel level, const std::string& tag, const std::string& message) override {
					std::lock_guard<std::mutex> lock(mutex_);
					if (!config_.enabled || level < config_.level) return;
					EmitLocked(level, FormatLineLocked(level, tag, NULL, 0, NULL, message));
				}

				void SetLevel(LogLevel level) override {
					std::lock_guard<std::mutex> lock(mutex_);
					config_.level = level;
					levelOverridden_ = true;
				}

				LogLevel GetLevel() const override {
					std::lock_guard<std::mutex> lock(mutex_);
					return config_.level;
				}

				void SetConfig(const LogConfig& config) override {
					std::lock_guard<std::mutex> lock(mutex_);
					config_ = config;
					configOverridden_ = true;
				}

				LogConfig GetConfig() const override {
					std::lock_guard<std::mutex> lock(mutex_);
					return config_;
				}

				// 应用配置源（文件键）的基线，保留运行期模块级覆盖（docs/10 §3.2.2 优先级）。
				void ApplyBaseConfig(const LogConfig& base) {
					std::lock_guard<std::mutex> lock(mutex_);
					if (configOverridden_) return;
					const LogLevel level = levelOverridden_ ? config_.level : base.level;
					config_ = base;
					config_.level = level;
					CleanupExpiredLocked(config_.retentionDays);
				}

				void Flush() {
					std::lock_guard<std::mutex> lock(mutex_);
					if (out_.is_open()) out_.flush();
				}

				LogStats SnapshotStats() const {
					std::lock_guard<std::mutex> lock(mutex_);
					return stats_;
				}

			private:
				// Format message with va_list
				std::string FormatMessage(const char* format, va_list args) {
					char buffer[4096];
					vsnprintf(buffer, sizeof(buffer), format, args);
					return std::string(buffer);
				}

				// Log with source location
				void LogWithLocation(LogLevel level, const char* file, int line,
					const char* function, const std::string& tag,
					const std::string& message) {

					std::lock_guard<std::mutex> lock(mutex_);
					if (!config_.enabled || level < config_.level) return;
					EmitLocked(level, FormatLineLocked(level, tag, file, line, function, message));
				}

				// 统一行格式（docs/10 §3.2.5）：
				// [UTC ms][LEVEL][Module][tid=N][trace][session][file:line function] msg
				std::string FormatLineLocked(LogLevel level, const std::string& tag,
					const char* file, int line, const char* function,
					const std::string& message) const {
					std::ostringstream oss;
					oss << '[' << platform::UtcNowIsoMillis() << "]["
						<< LogLevelToString(level) << "]["
						<< (tag.empty() ? name_ : tag) << "][tid=" << CurrentThreadId() << "]["
						<< (LogTraceId().empty() ? std::string("-") : LogTraceId()) << "]["
						<< (LogSessionId().empty() ? std::string("-") : LogSessionId()) << "][";
					if (file && file[0] != '\0') {
						std::string fileName = file;
						size_t pos = fileName.find_last_of("/\\");
						if (pos != std::string::npos) fileName = fileName.substr(pos + 1);
						oss << fileName << ':' << line << ' '
							<< (function ? function : "");
					} else {
						oss << '-';
					}
					oss << "] " << message << '\n';
					return oss.str();
				}

				// 调用方必须持有 mutex_。日志实现内禁止再打日志，只累加计数。
				void EmitLocked(LogLevel level, const std::string& logLine) {
					bool emitted = false;
					if (config_.toConsole) {
						std::cout << logLine;
						emitted = true;
					}
					if (config_.toFile) {
						if (WriteToFileLocked(level, logLine)) {
							emitted = true;
						} else {
							config_.toFile = false;   // 打不开就停用文件输出，控制台继续
							++stats_.writeFailures;
						}
					}
					if (emitted) ++stats_.linesWritten;
				}

				// 常驻流：懒打开、路径变化时重开；计数器与文件实际大小同步
				bool EnsureStreamLocked() {
					if (out_.is_open() && outPath_ == config_.filePath) return true;
					CloseStreamLocked();
					outPath_ = config_.filePath;
					out_.open(outPath_.c_str(), std::ios::out | std::ios::app | std::ios::binary);
					if (!out_.is_open()) {
						outPath_.clear();
						return false;
					}
					boost::system::error_code ec;
					const boost::uintmax_t sz = boost::filesystem::file_size(outPath_, ec);
					bytesWritten_ = ec ? 0 : static_cast<uint64_t>(sz);
					return true;
				}

				void CloseStreamLocked() {
					if (out_.is_open()) {
						out_.flush();
						out_.close();
					}
				}

				bool WriteToFileLocked(LogLevel level, const std::string& content) {
					if (!EnsureStreamLocked()) return false;

					// 内部字节计数判断轮转，不每行 stat（docs/10 §3.2.4）
					if (config_.maxFileSize > 0 &&
						bytesWritten_ + static_cast<uint64_t>(content.size()) > config_.maxFileSize) {
						CloseStreamLocked();
						try {
							RotateLogFile(config_.filePath);
						} catch (...) {
							++stats_.rotationFailures;
						}
						if (!EnsureStreamLocked()) return false;
					}

					out_.write(content.data(), static_cast<std::streamsize>(content.size()));
					// Warning 及以上立即落盘；Trace/Debug/Info 依赖缓冲
					if (static_cast<int>(level) >= static_cast<int>(LogLevel::Warning)) {
						out_.flush();
					}
					if (!out_.good()) {
						CloseStreamLocked();
						return false;
					}
					bytesWritten_ += static_cast<uint64_t>(content.size());
					return true;
				}

				// 只处理 <stem>.log / <stem>.<n>.log（docs/10 §3.2.4），保留 maxFiles 份总计。
				void RotateLogFile(const std::string& basePath) {
					namespace fs = boost::filesystem;
					const fs::path path(basePath);
					fs::path dir = path.parent_path();
					if (dir.empty()) dir = fs::path(".");
					const std::string stem = path.stem().string();

					const int archives = config_.maxFiles > 0 ? config_.maxFiles - 1 : 0;
					if (archives <= 0) {
						boost::system::error_code ec;
						fs::resize_file(basePath, 0, ec);
						if (ec) {
							ec.clear();
							fs::remove(basePath, ec);
						}
						return;
					}

					// 最老份先删，再自高到低后移；最高归档号为 archives（含当前共 maxFiles 份）
					{
						boost::system::error_code ec;
						fs::remove(dir / (stem + "." + std::to_string(archives + 1) + ".log"), ec);
					}
					for (int i = archives - 1; i >= 1; --i) {
						const fs::path from = dir / (stem + "." + std::to_string(i) + ".log");
						const fs::path to = dir / (stem + "." + std::to_string(i + 1) + ".log");
						boost::system::error_code ec;
						if (!fs::exists(from, ec)) continue;
						fs::remove(to, ec);
						ec.clear();
						fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
						if (!ec) fs::remove(from, ec);
					}

					// 当前文件 → .1.log，然后清空继续写
					const fs::path first = dir / (stem + ".1.log");
					boost::system::error_code ec;
					if (fs::exists(basePath, ec)) {
						fs::remove(first, ec);
						ec.clear();
						fs::copy_file(basePath, first, fs::copy_options::overwrite_existing, ec);
						if (ec) return;   // 拷贝失败：保持当前文件继续写
						ec.clear();
						fs::resize_file(basePath, 0, ec);
						if (ec) {
							ec.clear();
							fs::remove(basePath, ec);
						}
					}

					CleanupExpiredLocked(config_.retentionDays);
				}

				// 只清理本模块前缀的过期归档，绝不扫描删除别人的文件。
				void CleanupExpiredLocked(int retentionDays) {
					if (retentionDays <= 0) return;
					namespace fs = boost::filesystem;
					const fs::path path(config_.filePath);
					fs::path dir = path.parent_path();
					if (dir.empty()) dir = fs::path(".");
					const std::string stem = path.stem().string();
					const std::string currentName = stem + ".log";
					const std::time_t cutoff =
						std::time(NULL) - static_cast<std::time_t>(retentionDays) * 86400;

					boost::system::error_code ec;
					fs::directory_iterator it(dir, ec);
					if (ec) return;
					const fs::directory_iterator end;
					for (; it != end; it.increment(ec)) {
						if (ec) break;
						const std::string name = it->path().filename().string();
						if (name == currentName || !IsArchiveOf(name, stem)) continue;
						std::time_t mtime = 0;
						try {
							mtime = fs::last_write_time(it->path());
						} catch (...) {
							continue;
						}
						if (mtime < cutoff) {
							boost::system::error_code rmEc;
							fs::remove(it->path(), rmEc);
						}
					}
				}

				std::string name_;
				LogConfig config_;
				mutable std::mutex mutex_;      // L3
				std::ofstream out_;             // 常驻流（懒打开，Shutdown 关闭）
				std::string outPath_;           // 当前流对应路径（变更时重开）
				uint64_t bytesWritten_ = 0;     // 当前文件字节数（轮转判断）
				LogStats stats_;
				bool levelOverridden_ = false;  // SetLevel 后，Reload 不再改级别
				bool configOverridden_ = false; // SetConfig 后，Reload 不再改该 logger
			};

			// ============================================================
			// LogManager Implementation
			// ============================================================

			class LogManager : public ILogManager {
			public:
				~LogManager() override { Shutdown(); }

				ILogger* GetLogger(LogModule module) override {
					return GetLogger(std::string(GetModuleName(module)));
				}

				ILogger* GetLogger(const std::string& moduleName) override {
					const std::string key = LowerAscii(moduleName);
					std::lock_guard<std::mutex> lock(mutex_);
					std::map<std::string, std::unique_ptr<ModuleLogger>>::iterator it =
						loggers_.find(key);
					if (it != loggers_.end()) return it->second.get();

					std::unique_ptr<ModuleLogger> logger(
						new ModuleLogger(moduleName, MakeBaseConfigLocked(moduleName)));
					ModuleLogger* raw = logger.get();
					loggers_[key] = std::move(logger);
					return raw;
				}

				ILogger* GetGlobalLogger() override {
					return GetLogger(LogModule::Common);
				}

				Response Initialize(std::shared_ptr<IConfig> config,
									const std::string& logDir) override {
					const std::string dir = logDir.empty() ? std::string("logs") : logDir;

					// 先在锁外读配置：IConfig 内部可能打日志（L2 不可重入）
					std::vector<std::string> warnings;
					LogSettings settings = ReadLogSettings(config, &warnings);

					bool dirOk = EnsureDirectory(dir);
					if (!dirOk) {
						settings.toFile = false;
						warnings.push_back("log directory unavailable, file output disabled: " + dir);
					}

					{
						std::lock_guard<std::mutex> lock(mutex_);
						config_ = config;
						logDir_ = dir;
						settings_ = settings;
						toFileAllowed_ = dirOk;
						if (!dirOk) ++managerStats_.writeFailures;
						// 预热枚举模块 logger，配置变更有明确落点
						for (int m = 0; m < static_cast<int>(LogModule::Max); ++m) {
							const std::string name(GetModuleName(static_cast<LogModule>(m)));
							EnsureLoggerLocked(name);
						}
						ApplySettingsLocked();
					}

					for (size_t i = 0; i < warnings.size(); ++i) {
						GetLogger(LogModule::Base)->Warning("[Log] " + warnings[i]);
					}

					std::ostringstream msg;
					msg << "logging initialized: dir=" << dir
						<< " level=" << static_cast<int>(settings.level)
						<< " file=" << (settings.toFile ? 1 : 0)
						<< " size_mb=" << (settings.maxFileSize / (1024 * 1024))
						<< " max_files=" << settings.maxFiles
						<< " retention_days=" << settings.retentionDays;
					return Response::Success(msg.str());
				}

				Response Reload() override {
					std::shared_ptr<IConfig> config;
					{
						std::lock_guard<std::mutex> lock(mutex_);
						config = config_;
					}
					if (!config) {
						return Response::Success("logging not initialized");
					}

					std::vector<std::string> warnings;
					LogSettings settings = ReadLogSettings(config, &warnings);

					{
						std::lock_guard<std::mutex> lock(mutex_);
						settings_ = settings;
						// 目录可能在运行期被删除，重载时再确认一次
						toFileAllowed_ = EnsureDirectory(logDir_);
						ApplySettingsLocked();
					}

					for (size_t i = 0; i < warnings.size(); ++i) {
						GetLogger(LogModule::Base)->Warning("[Log] " + warnings[i]);
					}
					return Response::Success();
				}

				LogStats GetStats() const override {
					std::vector<const ModuleLogger*> snapshot;
					LogStats total;
					{
						std::lock_guard<std::mutex> lock(mutex_);
						total = managerStats_;
						for (std::map<std::string, std::unique_ptr<ModuleLogger>>::const_iterator
								 it = loggers_.begin(); it != loggers_.end(); ++it) {
							snapshot.push_back(it->second.get());
						}
					}

					for (size_t i = 0; i < snapshot.size(); ++i) {
						const LogStats s = snapshot[i]->SnapshotStats();
						total.linesWritten += s.linesWritten;
						total.writeFailures += s.writeFailures;
						total.rotationFailures += s.rotationFailures;
						total.droppedLines += s.droppedLines;
					}
					return total;
				}

				void Flush() override {
					std::vector<ModuleLogger*> snapshot;
					{
						std::lock_guard<std::mutex> lock(mutex_);
						for (std::map<std::string, std::unique_ptr<ModuleLogger>>::iterator
								 it = loggers_.begin(); it != loggers_.end(); ++it) {
							snapshot.push_back(it->second.get());
						}
					}
					for (size_t i = 0; i < snapshot.size(); ++i) {
						snapshot[i]->Flush();
					}
				}

				std::string GetLogDirectory() const override {
					std::lock_guard<std::mutex> lock(mutex_);
					return logDir_;
				}

				void Shutdown() override {
					std::map<std::string, std::unique_ptr<ModuleLogger>> loggers;
					{
						std::lock_guard<std::mutex> lock(mutex_);
						loggers.swap(loggers_);
						config_.reset();
					}
					// logger 在锁外析构（关闭文件）
				}

			private:
				LogConfig MakeBaseConfigLocked(const std::string& moduleName) const {
					LogConfig config;
					config.enabled = settings_.enabled;
					config.level = settings_.level;
					config.toFile = settings_.toFile && toFileAllowed_;
					config.toConsole = settings_.toConsole;
					config.filePath = logDir_ + "/" + moduleName + ".log";
					config.maxFileSize = settings_.maxFileSize;
					config.maxFiles = settings_.maxFiles;
					config.retentionDays = settings_.retentionDays;
					return config;
				}

				ModuleLogger* EnsureLoggerLocked(const std::string& moduleName) {
					const std::string key = LowerAscii(moduleName);
					std::map<std::string, std::unique_ptr<ModuleLogger>>::iterator it =
						loggers_.find(key);
					if (it != loggers_.end()) return it->second.get();
					std::unique_ptr<ModuleLogger> logger(
						new ModuleLogger(moduleName, MakeBaseConfigLocked(moduleName)));
					ModuleLogger* raw = logger.get();
					loggers_[key] = std::move(logger);
					return raw;
				}

				// L2 → L3：应用基线配置；模块级运行期覆盖由 ModuleLogger 自行保留
				void ApplySettingsLocked() {
					for (std::map<std::string, std::unique_ptr<ModuleLogger>>::iterator
							 it = loggers_.begin(); it != loggers_.end(); ++it) {
						it->second->ApplyBaseConfig(MakeBaseConfigLocked(it->second->Name()));
					}
				}

				mutable std::mutex mutex_;   // L2
				std::shared_ptr<IConfig> config_;
				std::string logDir_;
				LogSettings settings_;
				bool toFileAllowed_ = true;
				LogStats managerStats_;
				std::map<std::string, std::unique_ptr<ModuleLogger>> loggers_;
			};

			// ============================================================
			// Global singleton
			// ============================================================

			static LogManager g_logManager;

			ILogManager* GetLogManager() {
				return &g_logManager;
			}

	}
} // namespace vcs::base
