#pragma once

#include "conference_export.h"

#include <string>

namespace vcs {
	namespace conference {

		// ============================================================
		// Conference -> upper layer notification contract
		//
		// Owned by conference (L4) so that conference never includes absphone
		// (L5). absphone adapts this interface onto AbsPhoneObserver.
		//
		// Rules:
		//   1. Notifications only. Request/response style calls are API methods
		//      on IConferenceControl, never observer callbacks.
		//   2. Callbacks run on the per-conference EXECUTOR thread, never on the
		//      EventBus dispatch thread and never on the caller thread of a
		//      command. Order is the call order of the underlying events.
		//      Implementations must not block (they would stall media start/stop)
		//      and must marshal to their own thread before touching UI.
		//   3. New notifications get a default empty implementation here so
		//      existing observers keep compiling.
		// ============================================================
		// What the upper layer needs to know about one finished call.
		struct CallTerminationInfo {
			std::string reason;         // gateway-supplied cause, e.g. "Local hangup"
			std::string remoteUri;      // remote party as seen by the protocol layer
			std::string remoteAddress;  // remote media address, may be empty
			uint32_t durationSec = 0;   // talk time; 0 when never connected
		};

		// Diagnosable media failure report (docs/08 §2.9, decision 7).
		// Same fields as the SystemError payload conference publishes; the host
		// gets it here without subscribing to the bus.
		struct MediaErrorInfo {
			std::string stage;        // SessionNotActive / SetupResources / ...
			std::string op;           // SetupMediaResources / GetSessionTransports / StartMedia / StartTransport
			std::string conferenceId;
			std::string sessionId;
			int errorCode = 0;        // vcs::ErrorCode
			std::string message;      // lower-layer message, verbatim
			int64_t elapsedMs = 0;    // CallConnected -> failure
		};

		class VCS_CONFERENCE_API IConferenceObserver {
		public:
			virtual ~IConferenceObserver() = default;

			virtual void OnIncomingCall(const std::string& remoteUri, bool hasVideo) {
				(void)remoteUri;
				(void)hasVideo;
			}

			virtual void OnCallConnected(const std::string& remoteUri, bool hasVideo) {
				(void)remoteUri;
				(void)hasVideo;
			}

			virtual void OnCallTerminated(const CallTerminationInfo& info) {
				(void)info;
			}

			// Media start/stop failure. Must not block and must not call back
			// into conference (runs on the conference worker thread, same as
			// the notifications above).
			virtual void OnMediaError(const MediaErrorInfo& info) {
				(void)info;
			}
		};

	} // namespace conference
} // namespace vcs
