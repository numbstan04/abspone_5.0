#include "MediaFactory.h"
#include "MediaControl.h"
#include "MediaSdpNegotiator.h"
#include "IPlatformMediaFactory.h"
#include "ILogger.h"

#include <memory>

namespace vcs {
	namespace media {

		class MediaFactory::Impl {
		public:
			std::string name = "MediaFactory";
			std::string version = "1.0.0";
			bool initialized = false;
			std::shared_ptr<IPlatformMediaFactory> platformFactory;
		};

		MediaFactory::MediaFactory()
			: impl_(std::make_unique<Impl>()) {
		}

		MediaFactory::~MediaFactory() = default;

		const std::string& MediaFactory::GetName() const {
			return impl_->name;
		}

		const std::string& MediaFactory::GetVersion() const {
			return impl_->version;
		}

		bool MediaFactory::IsInitialized() const {
			return impl_->initialized;
		}

		std::unique_ptr<IMediaControl> MediaFactory::CreateMediaControl() {
			// 平台工厂只创建一次，多个 MediaControl 实例共享
			if (!impl_->platformFactory) {
				impl_->platformFactory = CreatePlatformMediaFactory();
				if (!impl_->platformFactory) {
					VCS_MEDIA_LOG_ERROR("[MediaFactory] CreatePlatformMediaFactory returned null; "
						"MediaControl will have no device manager");
				}
			}

			auto control = std::make_unique<MediaControl>(impl_->platformFactory);
			if (control) {
				impl_->initialized = true;
			}
			return control;
		}

		std::shared_ptr<vcs::media_config::IMediaSdpNegotiator>
		MediaFactory::CreateSdpNegotiator() {
			// 无状态：offer/answer 全部由入参决定，多个消费者可共用一个实例。
			return std::make_shared<MediaSdpNegotiator>();
		}

	} // namespace media
} // namespace vcs
