/**
 * @file WebRtcLogBridge.h
 * @brief Redirects libwebrtc logs into "<logDir>/webrtc.log".
 *
 * Platform-neutral by design: the file is opened with std::ofstream and the
 * dir comes from ILogManager::GetLogDirectory(), so the same code path is used
 * on Windows and Linux. Only the timestamp helper has a two-line
 * localtime_s/localtime_r difference.
 */

#pragma once

#ifdef VCS_WITH_WEBRTC

namespace vcs {
	namespace media {

		/// Install the libwebrtc log sink writing to "<logDir>/webrtc.log".
		/// Idempotent; safe to call from any thread. Does nothing when the log
		/// directory is unavailable.
		void StartWebRtcLogging();

		/// Remove the sink and close the file (optional; process exit does it).
		void StopWebRtcLogging();

	} // namespace media
} // namespace vcs

#endif // VCS_WITH_WEBRTC
