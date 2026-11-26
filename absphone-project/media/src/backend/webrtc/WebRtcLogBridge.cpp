/**
 * @file WebRtcLogBridge.cpp
 * @brief MSVC TU that owns the webrtc.log file and bridges it to libwebrtc.
 *
 * The libwebrtc LogSink itself lives in engine/LibWebRtcLogSink.cpp (clang-cl
 * + libc++). This file only opens the file (std::ofstream, works on Windows
 * and Linux) and hands a C function pointer to that sink through
 * IWebRtcLog.h, so no std:: type crosses the ABI boundary.
 */

#include "WebRtcLogBridge.h"

#ifdef VCS_WITH_WEBRTC

#include "webrtc/IWebRtcLog.h"

#include "ILogger.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

namespace vcs {
	namespace media {

		namespace {

			// webrtc::LoggingSeverity::LS_INFO (0=verbose,1=info,2=warning,3=error)
			constexpr int kMinSeverity = 1;

			std::mutex g_log_mu;
			std::ofstream g_log_file;
			bool g_active = false;

			std::string LogFilePath() {
				std::string dir;
				if (vcs::base::ILogManager* manager = vcs::base::GetLogManager()) {
					dir = manager->GetLogDirectory();
				}
				if (dir.empty()) dir = "logs";
				return dir + "/webrtc.log";
			}

			void FormatTimestamp(char* out, size_t out_size) {
				using namespace std::chrono;
				const system_clock::time_point now = system_clock::now();
				const milliseconds ms =
					duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
				const std::time_t seconds = system_clock::to_time_t(now);
				std::tm tm_value = {};
#ifdef _WIN32
				localtime_s(&tm_value, &seconds);
#else
				localtime_r(&seconds, &tm_value);
#endif
				std::snprintf(out, out_size,
					"%04d-%02d-%02d %02d:%02d:%02d.%03d",
					tm_value.tm_year + 1900, tm_value.tm_mon + 1, tm_value.tm_mday,
					tm_value.tm_hour, tm_value.tm_min, tm_value.tm_sec,
					static_cast<int>(ms.count()));
			}

			void WriteLine(void* /*user*/, int /*severity*/,
				const char* line, size_t length) {
				if (!line || length == 0) return;
				std::lock_guard<std::mutex> lock(g_log_mu);
				if (!g_log_file.is_open()) return;
				char stamp[32] = { 0 };
				FormatTimestamp(stamp, sizeof(stamp));
				g_log_file << '[' << stamp << "] ";
				g_log_file.write(line, static_cast<std::streamsize>(length));
				g_log_file << '\n';
				// Flush per line: webrtc tends to crash hard (RTC_CHECK), and a
				// buffered tail would be exactly what we need in that case.
				g_log_file.flush();
			}

		} // namespace

		void StartWebRtcLogging() {
			std::lock_guard<std::mutex> lock(g_log_mu);
			if (g_active) return;

			const std::string path = LogFilePath();
			std::ofstream file(path.c_str(),
				std::ios::out | std::ios::trunc | std::ios::binary);
			if (!file.is_open()) return;

			char stamp[32] = { 0 };
			FormatTimestamp(stamp, sizeof(stamp));
			file << "===== libwebrtc log start " << stamp << " =====\n";
			file.flush();

			g_log_file = std::move(file);
			g_active = true;

			VcsEnableWebRtcLogging(&WriteLine, nullptr, kMinSeverity);
		}

		void StopWebRtcLogging() {
			VcsDisableWebRtcLogging();
			std::lock_guard<std::mutex> lock(g_log_mu);
			if (g_log_file.is_open()) {
				g_log_file.flush();
				g_log_file.close();
			}
			g_active = false;
		}

	} // namespace media
} // namespace vcs

#endif // VCS_WITH_WEBRTC
