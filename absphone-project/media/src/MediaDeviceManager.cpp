#include "MediaDeviceManager.h"
#include "ILogger.h"

#include <exception>

namespace vcs {
	namespace media {

		MediaDeviceManager::MediaDeviceManager(
			std::shared_ptr<IPlatformMediaFactory> factory)
			: factory_(std::move(factory)) {
			EnumerateAll();
		}

		void MediaDeviceManager::EnumerateAll() {
			if (!factory_) {
				VCS_MEDIA_LOG_ERROR("[MediaDeviceManager] factory is null");
				return;
			}

			std::lock_guard<std::mutex> lock(mutex_);
			try {
				videoSourceInfos_ = factory_->EnumerateVideoSources();
				videoSinkInfos_ = factory_->EnumerateVideoSinks();
				audioSourceIds_ = factory_->EnumerateAudioSources();
				audioSinkIds_ = factory_->EnumerateAudioSinks();
			}
			catch (const std::exception& e) {
				VCS_MEDIA_LOG_ERROR("[MediaDeviceManager] EnumerateAll threw: %s", e.what());
				videoSourceInfos_.clear();
				videoSinkInfos_.clear();
				audioSourceIds_.clear();
				audioSinkIds_.clear();
				return;
			}

			VCS_MEDIA_LOG_INFO(
				"[MediaDeviceManager] Enumerated: videoSources=%zu, videoSinks=%zu, "
				"audioSources=%zu, audioSinks=%zu",
				videoSourceInfos_.size(), videoSinkInfos_.size(),
				audioSourceIds_.size(), audioSinkIds_.size());
		}

		std::string MediaDeviceManager::PickDefaultVideoSourceId(VideoSourceType type) const {
			for (const auto& info : videoSourceInfos_) {
				if (info.type == type) return info.id;
			}
			return videoSourceInfos_.empty() ? std::string() : videoSourceInfos_.front().id;
		}

		std::string MediaDeviceManager::PickDefaultVideoSinkId(VideoSinkType type) const {
			for (const auto& info : videoSinkInfos_) {
				if (info.type == type) return info.id;
			}
			return videoSinkInfos_.empty() ? std::string() : videoSinkInfos_.front().id;
		}

		std::shared_ptr<IVideoSource>
			MediaDeviceManager::GetVideoSource(const std::string& id) {
			if (!factory_) return nullptr;

			std::lock_guard<std::mutex> lock(mutex_);

			std::string actualId = id;
			if (actualId.empty()) {
				actualId = PickDefaultVideoSourceId(VideoSourceType::Camera);
				if (actualId.empty()) {
					VCS_MEDIA_LOG_WARNING("[MediaDeviceManager] No video source available");
					return nullptr;
				}
			}

			auto it = videoSources_.find(actualId);
			if (it != videoSources_.end()) return it->second;

			auto src = factory_->CreateVideoSource(actualId);
			if (!src) {
				VCS_MEDIA_LOG_ERROR("[MediaDeviceManager] CreateVideoSource failed: id=%s",
					actualId.c_str());
				return nullptr;
			}
			videoSources_[actualId] = src;
			VCS_MEDIA_LOG_INFO("[MediaDeviceManager] Created video source: id=%s",
				actualId.c_str());
			return src;
		}

		std::shared_ptr<IAudioSource>
			MediaDeviceManager::GetAudioSource(const std::string& id) {
			if (!factory_) return nullptr;

			std::lock_guard<std::mutex> lock(mutex_);

			std::string actualId = id;
			if (actualId.empty()) {
				if (audioSourceIds_.empty()) {
					VCS_MEDIA_LOG_WARNING("[MediaDeviceManager] No audio source available");
					return nullptr;
				}
				actualId = audioSourceIds_.front();
			}

			auto it = audioSources_.find(actualId);
			if (it != audioSources_.end()) return it->second;

			auto src = factory_->CreateAudioSource(actualId);
			if (!src) {
				VCS_MEDIA_LOG_ERROR("[MediaDeviceManager] CreateAudioSource failed: id=%s",
					actualId.c_str());
				return nullptr;
			}
			audioSources_[actualId] = src;
			VCS_MEDIA_LOG_INFO("[MediaDeviceManager] Created audio source: id=%s",
				actualId.c_str());
			return src;
		}

		std::shared_ptr<IVideoSink>
			MediaDeviceManager::GetVideoSink(
				const std::string& id,
				const vcs::media_config::VideoSinkConfig& cfg) {
			if (!factory_) return nullptr;

			std::lock_guard<std::mutex> lock(mutex_);

			std::string actualId = id;
			if (actualId.empty()) {
				actualId = PickDefaultVideoSinkId(VideoSinkType::MainWindow);
				if (actualId.empty()) {
					VCS_MEDIA_LOG_WARNING("[MediaDeviceManager] No video sink available");
					return nullptr;
				}
			}

			auto it = videoSinks_.find(actualId);
			if (it != videoSinks_.end()) return it->second;

			auto sink = factory_->CreateVideoSink(actualId, cfg);
			if (!sink) {
				VCS_MEDIA_LOG_ERROR("[MediaDeviceManager] CreateVideoSink failed: id=%s",
					actualId.c_str());
				return nullptr;
			}
			videoSinks_[actualId] = sink;
			VCS_MEDIA_LOG_INFO("[MediaDeviceManager] Created video sink: id=%s",
				actualId.c_str());
			return sink;
		}

		std::shared_ptr<IAudioSink>
			MediaDeviceManager::GetAudioSink(
				const std::string& id,
				const vcs::media_config::AudioSinkConfig& cfg) {
			if (!factory_) return nullptr;

			std::lock_guard<std::mutex> lock(mutex_);

			std::string actualId = id;
			if (actualId.empty()) {
				if (audioSinkIds_.empty()) {
					VCS_MEDIA_LOG_WARNING("[MediaDeviceManager] No audio sink available");
					return nullptr;
				}
				actualId = audioSinkIds_.front();
			}

			auto it = audioSinks_.find(actualId);
			if (it != audioSinks_.end()) return it->second;

			auto sink = factory_->CreateAudioSink(actualId, cfg);
			if (!sink) {
				VCS_MEDIA_LOG_ERROR("[MediaDeviceManager] CreateAudioSink failed: id=%s",
					actualId.c_str());
				return nullptr;
			}
			audioSinks_[actualId] = sink;
			VCS_MEDIA_LOG_INFO("[MediaDeviceManager] Created audio sink: id=%s",
				actualId.c_str());
			return sink;
		}

		std::shared_ptr<IVideoSource>
			MediaDeviceManager::GetDefaultVideoSource(VideoSourceType type) {
			std::string id;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				id = PickDefaultVideoSourceId(type);
			}
			if (id.empty()) return nullptr;
			return GetVideoSource(id);
		}

		std::shared_ptr<IVideoSink>
			MediaDeviceManager::GetDefaultVideoSink(VideoSinkType type) {
			std::string id;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				id = PickDefaultVideoSinkId(type);
			}
			if (id.empty()) return nullptr;
			return GetVideoSink(id, vcs::media_config::VideoSinkConfig{});
		}

		std::vector<VideoSourceInfo> MediaDeviceManager::ListVideoSources() const {
			std::lock_guard<std::mutex> lock(mutex_);
			return videoSourceInfos_;
		}

		std::vector<std::string> MediaDeviceManager::ListAudioSources() const {
			std::lock_guard<std::mutex> lock(mutex_);
			return audioSourceIds_;
		}

		std::vector<VideoSinkInfo> MediaDeviceManager::ListVideoSinks() const {
			std::lock_guard<std::mutex> lock(mutex_);
			return videoSinkInfos_;
		}

		std::vector<std::string> MediaDeviceManager::ListAudioSinks() const {
			std::lock_guard<std::mutex> lock(mutex_);
			return audioSinkIds_;
		}

	}
} // namespace vcs::media
