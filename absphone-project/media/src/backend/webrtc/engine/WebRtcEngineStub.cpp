/**
 * @file WebRtcEngineStub.cpp
 * @brief Fallback factory used when the libwebrtc adapter is not compiled in.
 *
 * The default project build (VS2017 / non-libwebrtc toolchain) cannot compile
 * m140 headers, so VCS_WITH_WEBRTC=ON still builds the version-neutral middle
 * layer plus this stub. Configure with -DVCS_WEBRTC_BUILD_LIBWEBRTC=ON to
 * replace this file with engine/LibWebRtcEngine.cpp in a matching toolchain.
 */

#include "webrtc/IWebRtcEngine.h"

#ifdef VCS_WITH_WEBRTC

namespace vcs {
	namespace webrtc_engine {

		std::unique_ptr<IWebRtcEngine> CreateWebRtcEngine() {
			// WebRTC transport is unavailable in this build. Callers must treat
			// nullptr as "WebRTC not linked" and fall back / report an error.
			return nullptr;
		}

	} // namespace webrtc_engine
} // namespace vcs

#endif // VCS_WITH_WEBRTC
