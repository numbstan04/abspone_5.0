#pragma once

#include "IPlatformMediaFactory.h"
#include "IVideoSource.h"
#include "IAudioSource.h"
#include "IVideoSink.h"
#include "IAudioSink.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vcs {
	namespace media {

		/**
		 * @brief 媒体设备管理器（媒体模块内部，上层不可见）
		 *
		 * 持有平台工厂，懒创建设备对象并缓存，提供默认设备选择。
		 * 设备对象的 shared_ptr 由本管理器持有；流只持有 shared_ptr 副本。
		 */
		class MediaDeviceManager {
		public:
			explicit MediaDeviceManager(std::shared_ptr<IPlatformMediaFactory> factory);
			~MediaDeviceManager() = default;

			MediaDeviceManager(const MediaDeviceManager&) = delete;
			MediaDeviceManager& operator=(const MediaDeviceManager&) = delete;

			void EnumerateAll();

			std::shared_ptr<IVideoSource> GetVideoSource(const std::string& id);
			std::shared_ptr<IAudioSource> GetAudioSource(const std::string& id);
			std::shared_ptr<IVideoSink>   GetVideoSink(
				const std::string& id,
				const vcs::media_config::VideoSinkConfig& cfg);
			std::shared_ptr<IAudioSink>   GetAudioSink(
				const std::string& id,
				const vcs::media_config::AudioSinkConfig& cfg);

			std::shared_ptr<IVideoSource> GetDefaultVideoSource(VideoSourceType type);
			std::shared_ptr<IVideoSink>   GetDefaultVideoSink(VideoSinkType type);

			std::vector<VideoSourceInfo> ListVideoSources() const;
			std::vector<std::string>     ListAudioSources() const;
			std::vector<VideoSinkInfo>   ListVideoSinks() const;
			std::vector<std::string>     ListAudioSinks() const;

		private:
			std::string PickDefaultVideoSourceId(VideoSourceType type) const;
			std::string PickDefaultVideoSinkId(VideoSinkType type) const;

			std::shared_ptr<IPlatformMediaFactory> factory_;
			mutable std::mutex mutex_;

			std::vector<VideoSourceInfo> videoSourceInfos_;
			std::vector<VideoSinkInfo>   videoSinkInfos_;
			std::vector<std::string>     audioSourceIds_;
			std::vector<std::string>     audioSinkIds_;

			std::unordered_map<std::string, std::shared_ptr<IVideoSource>> videoSources_;
			std::unordered_map<std::string, std::shared_ptr<IAudioSource>> audioSources_;
			std::unordered_map<std::string, std::shared_ptr<IVideoSink>>   videoSinks_;
			std::unordered_map<std::string, std::shared_ptr<IAudioSink>>   audioSinks_;
		};

	}
} // namespace vcs::media
