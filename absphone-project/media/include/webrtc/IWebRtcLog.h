/**
 * @file IWebRtcLog.h
 * @brief ABI-neutral hook that forwards libwebrtc log lines to 5.0 logging.
 *
 * [Why]
 * libwebrtc writes to its own LogSink chain (rtc_base/logging.h). That sink
 * interface uses std::string and must be implemented inside a libc++ TU, so
 * this header exposes a plain C interface that any MSVC / GCC / Clang TU can
 * call. The implementation lives in engine/LibWebRtcLogSink.cpp (clang-cl on
 * Windows, the normal libwebrtc toolchain on Linux) and writes into whatever
 * file the caller opened - typically "<logDir>/webrtc.log".
 *
 * [Threading]
 * The callback may be invoked concurrently from any libwebrtc thread; the
 * receiver is responsible for serialising its own writes.
 *
 * [Platform]
 * Deliberately platform-neutral: only scalar types, a function pointer and a
 * user cookie cross the boundary, so the same code works on Windows and Linux.
 */

#pragma once

#ifdef VCS_WITH_WEBRTC

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

	/// Receives one already formatted libwebrtc log line (no trailing newline).
	/// `severity` follows webrtc::LoggingSeverity: 0 verbose, 1 info,
	/// 2 warning, 3 error (4 = none).
	typedef void (*VcsWebRtcLogWriteFn)(void* user, int severity,
		const char* line, size_t length);

	/// Install the sink. `min_severity` is a webrtc::LoggingSeverity value.
	/// Calling it twice replaces the previous sink.
	void VcsEnableWebRtcLogging(VcsWebRtcLogWriteFn write_fn, void* user,
		int min_severity);

	/// Remove the sink (safe to call when none is installed).
	void VcsDisableWebRtcLogging(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VCS_WITH_WEBRTC
