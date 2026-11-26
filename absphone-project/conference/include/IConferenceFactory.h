#pragma once

#include "conference_export.h"
#include "IModuleFactory.h"
#include "IConferenceControl.h"

#include <memory>

namespace vcs {
	namespace gateway {
		class IGatewayManager;
	}
	namespace media {
		class IMediaControl;
	}

	namespace conference {

		// ============================================================
		// Conference module factory.
		//
		// absphone uses this interface to obtain the call-control facade. The
		// implementation wires the control to the gateway manager; absphone
		// never touches concrete conference classes.
		// ============================================================
		class VCS_CONFERENCE_API IConferenceFactory : public common::IModuleFactory {
		public:
			virtual ~IConferenceFactory() = default;

			// `gateway` / `media` are non-owning and must outlive the returned
			// control. Media orchestration lives in the conference layer
			// (docs/08 §2.3), so media is injected here, not into gateway.
			virtual std::unique_ptr<IConferenceControl> CreateConferenceControl(
				vcs::gateway::IGatewayManager* gateway,
				vcs::media::IMediaControl* media) = 0;
		};

	} // namespace conference
} // namespace vcs
