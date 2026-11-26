/**
 * @file WebRtcMediaEngineStub.cpp
 * @brief Fallback factory used when the libwebrtc adapter is not compiled in.
 *
 * Without VCS_WEBRTC_BUILD_LIBWEBRTC the engine cannot be built (m140 needs
 * Chromium clang-cl + libc++), so WebRtcRtpBackend falls back to the native
 * RTP path and keeps the RTP-codec-only behaviour.
 */

#include "webrtc/IWebRtcMediaEngine.h"

#ifdef VCS_WITH_WEBRTC

#include "webrtc/IWebRtcLog.h"

namespace vcs {
	namespace webrtc_engine {

		IWebRtcMediaEngine* CreateWebRtcMediaEngine() {
			return nullptr;
		}
		void DestroyWebRtcMediaEngine(IWebRtcMediaEngine* engine) {
			delete engine;
		}

	} // namespace webrtc_engine
} // namespace vcs

extern "C" {

	/// No libwebrtc adapter in this build: nothing to forward.
	void VcsEnableWebRtcLogging(VcsWebRtcLogWriteFn /*write_fn*/, void* /*user*/,
		int /*min_severity*/) {
	}

	void VcsDisableWebRtcLogging(void) {
	}

} // extern "C"

#endif // VCS_WITH_WEBRTC
