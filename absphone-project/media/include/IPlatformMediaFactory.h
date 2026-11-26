#pragma once

#include "MediaConfig.h"   // VideoSinkConfig / AudioSinkConfig
#include <memory>
#include <string>
#include <vector>

namespace vcs {
	namespace media {

		class IVideoSource;
		class IAudioSource;
		class IVideoSink;
		class IAudioSink;

		// ============================================================
		// 设备类别
		// ============================================================
		enum class VideoSourceType {
			Camera,          // 摄像头（主流）
			ScreenCapture,   // 屏幕/窗口捕获（辅流）
			File,            // 文件播放（测试）
		};

		enum class VideoSinkType {
			MainWindow,      // 主窗口（主流）
			SecondaryWindow, // 辅窗口 / 画中画（辅流）
			FullScreen,      // 全屏
		};

		// ============================================================
		// 设备信息（枚举结果）
		// ============================================================
		struct VideoSourceInfo {
			std::string id;              // "camera:0" / "screen:0"
			VideoSourceType type = VideoSourceType::Camera;
			std::string displayName;
		};

		struct VideoSinkInfo {
			std::string id;              // "main_window" / "secondary_window"
			VideoSinkType type = VideoSinkType::MainWindow;
			std::string displayName;
		};

		/**
		 * @brief 平台媒体工厂（平台层实现，媒体模块调用）
		 *
		 * 与 ICodecFactory 对称：平台层只负责“枚举 + 创建对象”，
		 * 媒体模块负责注册表、路由、生命周期。
		 */
		class IPlatformMediaFactory {
		public:
			virtual ~IPlatformMediaFactory() = default;

			virtual std::vector<VideoSourceInfo> EnumerateVideoSources() = 0;
			virtual std::vector<std::string>     EnumerateAudioSources() = 0;
			virtual std::vector<VideoSinkInfo>   EnumerateVideoSinks() = 0;
			virtual std::vector<std::string>     EnumerateAudioSinks() = 0;

			virtual std::shared_ptr<IVideoSource> CreateVideoSource(
				const std::string& deviceId) = 0;
			virtual std::shared_ptr<IAudioSource> CreateAudioSource(
				const std::string& deviceId) = 0;
			virtual std::shared_ptr<IVideoSink> CreateVideoSink(
				const std::string& sinkId,
				const vcs::media_config::VideoSinkConfig& config) = 0;
			virtual std::shared_ptr<IAudioSink> CreateAudioSink(
				const std::string& sinkId,
				const vcs::media_config::AudioSinkConfig& config) = 0;
		};

		/**
		 * @brief 平台层入口，类似 CreateDefaultCodecFactory()
		 */
		std::shared_ptr<IPlatformMediaFactory> CreatePlatformMediaFactory();

	}
} // namespace vcs::media
