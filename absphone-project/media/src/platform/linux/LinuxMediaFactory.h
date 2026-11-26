#pragma once

#include "IPlatformMediaFactory.h"

namespace vcs {
	namespace media {
		namespace platform {
			namespace linux {

				/**
				 * @brief Linux 平台媒体工厂
				 *
				 * 已实现（采集）：
				 *   - 视频：V4L2 摄像头（V4L2VideoSource），id = camera:N → /dev/videoN
				 *   - 音频：ALSA 麦克风（AlsaAudioSource），id = mic:default / mic:N
				 *
				 * 未实现（渲染）：屏幕捕获、视频窗口、音频播放。
				 * 枚举返回空列表、创建返回 nullptr，由上层走"无汇"路径；
				 * 这与设备缺失是同一种表现，不影响信令、编解码、RTP 与传输。
				 */
				class LinuxMediaFactory : public IPlatformMediaFactory {
				public:
					LinuxMediaFactory();
					~LinuxMediaFactory() override;

					std::vector<VideoSourceInfo> EnumerateVideoSources() override;
					std::vector<std::string>     EnumerateAudioSources() override;
					std::vector<VideoSinkInfo>   EnumerateVideoSinks() override;
					std::vector<std::string>     EnumerateAudioSinks() override;

					std::shared_ptr<IVideoSource> CreateVideoSource(
						const std::string& deviceId) override;
					std::shared_ptr<IAudioSource> CreateAudioSource(
						const std::string& deviceId) override;
					std::shared_ptr<IVideoSink> CreateVideoSink(
						const std::string& sinkId,
						const vcs::media_config::VideoSinkConfig& config) override;
					std::shared_ptr<IAudioSink> CreateAudioSink(
						const std::string& sinkId,
						const vcs::media_config::AudioSinkConfig& config) override;
				};

			}
		}
	}
}
