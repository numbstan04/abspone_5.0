/**
 * @file LibWebRtcLogSink.cpp
 * @brief libwebrtc LogSink implementation forwarding to 5.0 (clang-cl TU).
 *
 * *** COMPILED WITH CHROMIUM CLANG-CL + LIBC++ ON WINDOWS ***
 * webrtc::LogSink takes std::string, so it cannot be implemented in an MSVC
 * TU. Everything here is platform-neutral; on Linux the same file builds with
 * the normal libwebrtc toolchain.
 */

#include "webrtc/IWebRtcLog.h"

#ifdef VCS_WITH_WEBRTC

#include <memory>
#include <mutex>
#include <string>

#include "rtc_base/logging.h"

namespace vcs {
	namespace webrtc_engine {

		namespace {

			class BridgeLogSink : public webrtc::LogSink {
			public:
				BridgeLogSink(VcsWebRtcLogWriteFn write_fn, void* user)
					: write_fn_(write_fn), user_(user) {
				}

				void OnLogMessage(const std::string& message) override {
					Emit(message, webrtc::LS_INFO, nullptr);
				}

				void OnLogMessage(const std::string& message,
					webrtc::LoggingSeverity severity,
					const char* tag) override {
					Emit(message, severity, tag);
				}

			private:
				void Emit(const std::string& message,
					webrtc::LoggingSeverity severity, const char* tag) {
					if (!write_fn_) return;

					char prefix[8] = { '[', '?', ']', ' ', '\0' };
					switch (severity) {
					case webrtc::LS_VERBOSE:   prefix[1] = 'V'; break;
					case webrtc::LS_INFO:      prefix[1] = 'I'; break;
					case webrtc::LS_WARNING:   prefix[1] = 'W'; break;
					case webrtc::LS_ERROR:     prefix[1] = 'E'; break;
					default:                   prefix[1] = '-'; break;
					}
					(void)tag;

					std::string line(prefix);
					line.append(message);
					write_fn_(user_, static_cast<int>(severity),
						line.c_str(), line.size());
				}

				VcsWebRtcLogWriteFn write_fn_ = nullptr;
				void* user_ = nullptr;
			};

			std::mutex g_sink_mu;
			std::unique_ptr<BridgeLogSink> g_sink;

		} // namespace

	} // namespace webrtc_engine
} // namespace vcs

extern "C" {

	void VcsEnableWebRtcLogging(VcsWebRtcLogWriteFn write_fn, void* user,
		int min_severity) {
		std::lock_guard<std::mutex> lock(vcs::webrtc_engine::g_sink_mu);
		if (vcs::webrtc_engine::g_sink) {
			webrtc::LogMessage::RemoveLogToStream(
				vcs::webrtc_engine::g_sink.get());
			vcs::webrtc_engine::g_sink.reset();
		}
		if (!write_fn) return;

		vcs::webrtc_engine::g_sink =
			std::make_unique<vcs::webrtc_engine::BridgeLogSink>(write_fn, user);
		webrtc::LogMessage::AddLogToStream(
			vcs::webrtc_engine::g_sink.get(),
			static_cast<webrtc::LoggingSeverity>(min_severity));
	}

	void VcsDisableWebRtcLogging(void) {
		std::lock_guard<std::mutex> lock(vcs::webrtc_engine::g_sink_mu);
		if (vcs::webrtc_engine::g_sink) {
			webrtc::LogMessage::RemoveLogToStream(
				vcs::webrtc_engine::g_sink.get());
			vcs::webrtc_engine::g_sink.reset();
		}
	}

} // extern "C"

#endif // VCS_WITH_WEBRTC
