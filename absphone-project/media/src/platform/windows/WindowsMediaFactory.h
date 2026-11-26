#pragma once

#include "IPlatformMediaFactory.h"

namespace vcs {
	namespace media {
		namespace platform {
			namespace windows {

				/**
				 * @brief Windows 平台媒体工厂
				 *
				 * 实现：
				 *   - 视频采集源：MediaFoundation 摄像头（MFVideoSource）、
				 *                 DXGI Desktop Duplication 屏幕捕获（ScreenCaptureSource）
				 *   - 音频采集源：WASAPI 麦克风（WasapiAudioSource）
				 *   - 视频渲染：GDI StretchBlt 到 HWND（GdiVideoSink）
				 *   - 音频渲染：WASAPI 扬声器（WasapiAudioSink）
				 */
				class WindowsMediaFactory : public IPlatformMediaFactory {
				public:
					WindowsMediaFactory();
					~WindowsMediaFactory() override;

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