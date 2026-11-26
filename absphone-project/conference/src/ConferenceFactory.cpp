#include "ConferenceFactory.h"
#include "ConferenceControl.h"

#include <exception>
#include <memory>

namespace vcs {
	namespace conference {

		class ConferenceFactory::Impl {
		public:
			std::string name = "ConferenceFactory";
			std::string version = "1.0.0";
			bool initialized = false;
		};

		ConferenceFactory::ConferenceFactory()
			: impl_(std::make_unique<Impl>()) {
		}

		ConferenceFactory::~ConferenceFactory() = default;

		const std::string& ConferenceFactory::GetName() const {
			return impl_->name;
		}

		const std::string& ConferenceFactory::GetVersion() const {
			return impl_->version;
		}

		bool ConferenceFactory::IsInitialized() const {
			return impl_->initialized;
		}

		std::unique_ptr<IConferenceControl> ConferenceFactory::CreateConferenceControl(
			vcs::gateway::IGatewayManager* gateway,
			vcs::media::IMediaControl* media) {
			return std::make_unique<ConferenceControl>(gateway, media);
		}

	} // namespace conference
} // namespace vcs
