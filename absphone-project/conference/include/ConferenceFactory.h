#pragma once

#include "conference_export.h"
#include "IConferenceFactory.h"

#include <memory>
#include <string>

namespace vcs {
	namespace conference {

		class ConferenceFactory : public IConferenceFactory {
		public:
			ConferenceFactory();
			~ConferenceFactory() override;

			const std::string& GetName() const override;
			const std::string& GetVersion() const override;
			bool IsInitialized() const override;

			std::unique_ptr<IConferenceControl> CreateConferenceControl(
				vcs::gateway::IGatewayManager* gateway,
				vcs::media::IMediaControl* media) override;

		private:
			struct Impl;
			std::unique_ptr<Impl> impl_;
		};

	} // namespace conference
} // namespace vcs
