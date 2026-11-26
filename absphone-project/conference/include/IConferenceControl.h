#pragma once

#include "conference_export.h"
#include "Response.h"
#include "MediaInfoSnapshot.h"
#include "EventBus.h"

#include <memory>
#include <string>

namespace vcs {
	namespace conference {

		class IConferenceObserver;

		// ============================================================
		// Terminal call control, called by absphone.
		//
		// Dependency direction: absphone (L5) -> conference (L4) -> gateway (L3).
		// conference never references absphone types; notifications go out via
		// IConferenceObserver, platform facts come in via IEventBus.
		//
		// Ownership: created by IConferenceFactory, owned by absphone.
		// Call order: SetEventBus() -> SetObserver() -> Initialize().
		// ============================================================
		class VCS_CONFERENCE_API IConferenceControl {
		public:
			virtual ~IConferenceControl() = default;

			// Subscribe to platform events. Must be called before Initialize().
			virtual void SetEventBus(std::shared_ptr<vcs::IEventBus> bus) = 0;

			// Upper-layer notifications. Non-owning; may be nullptr.
			virtual void SetObserver(IConferenceObserver* observer) = 0;

			// Starts event subscriptions. Requires SetEventBus().
			virtual Response Initialize() = 0;

			// docs/08 §3（决策 7）：关机前收口，幂等，不等待、不加线程。
			//   1) 标记 stopped_（在途 handler 直接返回）
			//   2) 退订事件
			//   3) 停当前会议的传输与媒体（幂等）
			// 默认空实现：既有实现方无需改动。
			virtual void Shutdown() {}

			// ============================================================
			// Terminal business control (called by absphone)
			// ============================================================

			virtual Response MakeCall(const std::string& remoteUri, bool enableVideo) = 0;
			virtual Response HangupCall() = 0;
			virtual Response AnswerCall(bool accept) = 0;
			virtual Response SendDTMF(const std::string& digits) = 0;

			// ============================================================
			// Media information (troubleshooting)
			//
			// Pull model, copy-out: fills `out` with the current call's
			// audio / video / secondary-video snapshot. Callable from any
			// thread (the host polls it about once per second); it never
			// enters media dispatch and never hands out a live pointer.
			//
			//   Success    `out` filled; out.live == false means the call has
			//              ended and this is the final snapshot, still valid
			//              until the next call starts.
			//   NotFound   no call has ever run on this control.
			//
			// Defaulted so existing implementations keep compiling; it is
			// never a "feature" to be added later - the media layer retains
			// the negotiation result and the per-stream counters from the
			// first packet, so the data exists whether or not anyone relays it.
			// ============================================================
			virtual Response GetMediaInfo(vcs::MediaInfoSnapshot& out) {
				(void)out;
				return Response::Error(ResponseCode::NotImplemented,
					"media info not supported by this implementation");
			}
		};

	} // namespace conference
} // namespace vcs
