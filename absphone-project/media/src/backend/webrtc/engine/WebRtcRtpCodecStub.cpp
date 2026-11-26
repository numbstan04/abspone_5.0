/**
 * @file WebRtcRtpCodecStub.cpp
 * @brief Fallback factory used when the libwebrtc adapter is not compiled in.
 *
 * The default project build (VS2017 / non-libwebrtc toolchain) cannot compile
 * m140 headers, so VCS_WITH_WEBRTC=ON still builds the version-neutral middle
 * layer plus this stub. Configure with -DVCS_WEBRTC_BUILD_LIBWEBRTC=ON to
 * replace this file with engine/LibWebRtcRtpCodec.cpp in a matching toolchain.
 */

#include "webrtc/IWebRtcRtpCodec.h"

#ifdef VCS_WITH_WEBRTC

namespace vcs {
	namespace webrtc_engine {

		IWebRtcRtpCodec* CreateWebRtcRtpCodec() {
			// No libwebrtc in this build. WebRtcRtpBackend then keeps the
			// NativeRtpBackend H.264 packetization/depacketization.
			return nullptr;
		}

		void DestroyWebRtcRtpCodec(IWebRtcRtpCodec* codec) {
			delete codec;
		}

	} // namespace webrtc_engine
} // namespace vcs

#endif // VCS_WITH_WEBRTC
