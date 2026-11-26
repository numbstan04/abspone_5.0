// Linux 平台层：采集已实现（V4L2 摄像头 + ALSA 麦克风），渲染尚未实现。
//
// 渲染（屏幕捕获、视频窗口、音频播放）返回 nullptr/空列表，由上层走
// "无汇"路径；这与设备缺失是同一种表现，不影响信令、编解码、RTP 与传输。
// 采集与渲染是 docs/06 §6 里分开列的两件事，这里只补采集。
#include "LinuxMediaFactory.h"
#include "AlsaAudioSource.h"
#include "V4L2VideoSource.h"
#include "ILogger.h"

#include <atomic>
#include <cstdlib>

namespace vcs {
	namespace media {
		namespace platform {
			namespace linux {

				namespace {

					// 渲染未实现只提示一次，避免每路流启动都刷屏
					void WarnOnce(std::atomic<bool>& flag, const char* what) {
						if (!flag.exchange(true)) {
							VCS_MEDIA_LOG_WARNING(
								"[LinuxMediaFactory] %s is not implemented on Linux yet; "
								"capture, signaling, codecs and RTP are unaffected", what);
						}
					}

				} // anonymous namespace

				LinuxMediaFactory::LinuxMediaFactory() {
					VCS_MEDIA_LOG_INFO(
						"[LinuxMediaFactory] created: capture=ALSA+V4L2, render=absent");
				}

				LinuxMediaFactory::~LinuxMediaFactory() = default;

				// ============================================================
				// 枚举
				// ============================================================
				std::vector<VideoSourceInfo> LinuxMediaFactory::EnumerateVideoSources() {
					// 屏幕捕获未实现，列表里只有摄像头
					return EnumerateVideoCaptureDevices();
				}

				std::vector<std::string> LinuxMediaFactory::EnumerateAudioSources() {
					const std::vector<std::string> devices = EnumerateAudioCaptureDevices();
					std::vector<std::string> result;
					result.reserve(devices.size());
					for (size_t i = 0; i < devices.size(); ++i) {
						result.push_back("mic:" + std::to_string(i));
						VCS_MEDIA_LOG_INFO("[LinuxMediaFactory] audio source[%zu]: mic:%zu -> %s",
							i, i, devices[i].c_str());
					}
					return result;
				}

				std::vector<VideoSinkInfo> LinuxMediaFactory::EnumerateVideoSinks() {
					return std::vector<VideoSinkInfo>();
				}

				std::vector<std::string> LinuxMediaFactory::EnumerateAudioSinks() {
					return std::vector<std::string>();
				}

				// ============================================================
				// CreateVideoSource / CreateAudioSource
				// ============================================================
				std::shared_ptr<IVideoSource>
					LinuxMediaFactory::CreateVideoSource(const std::string& deviceId) {
					if (deviceId.rfind("camera:", 0) == 0) {
						const int index = std::atoi(deviceId.c_str() + 7);
						if (index < 0) {
							VCS_MEDIA_LOG_ERROR(
								"[LinuxMediaFactory] CreateVideoSource: bad camera index: %s",
								deviceId.c_str());
							return nullptr;
						}
						return std::make_shared<V4L2VideoSource>(index);
					}
					if (deviceId.rfind("screen:", 0) == 0) {
						VCS_MEDIA_LOG_WARNING(
							"[LinuxMediaFactory] screen capture is not implemented on Linux yet: %s",
							deviceId.c_str());
						return nullptr;
					}
					VCS_MEDIA_LOG_ERROR(
						"[LinuxMediaFactory] CreateVideoSource: unknown id=%s", deviceId.c_str());
					return nullptr;
				}

				std::shared_ptr<IAudioSource>
					LinuxMediaFactory::CreateAudioSource(const std::string& deviceId) {
					// 必须在 mic:N 之前判掉：atoi("default") == 0，否则会静默
					// 退回 mic:0。
					if (deviceId == "mic:default") {
						return std::make_shared<AlsaAudioSource>("default", deviceId);
					}
					if (deviceId.rfind("mic:", 0) == 0) {
						const int index = std::atoi(deviceId.c_str() + 4);
						if (index < 0) {
							VCS_MEDIA_LOG_ERROR(
								"[LinuxMediaFactory] CreateAudioSource: bad mic index: %s",
								deviceId.c_str());
							return nullptr;
						}
						const std::vector<std::string> devices = EnumerateAudioCaptureDevices();
						if (static_cast<size_t>(index) >= devices.size()) {
							VCS_MEDIA_LOG_ERROR(
								"[LinuxMediaFactory] %s not found (enumerated=%zu)",
								deviceId.c_str(), devices.size());
							return nullptr;
						}
						return std::make_shared<AlsaAudioSource>(
							devices[static_cast<size_t>(index)], deviceId);
					}
					VCS_MEDIA_LOG_ERROR(
						"[LinuxMediaFactory] CreateAudioSource: unknown id=%s", deviceId.c_str());
					return nullptr;
				}

				// ============================================================
				// CreateVideoSink / CreateAudioSink（渲染：未实现）
				// ============================================================
				std::shared_ptr<IVideoSink>
					LinuxMediaFactory::CreateVideoSink(
						const std::string& sinkId,
						const vcs::media_config::VideoSinkConfig& /*config*/) {
					static std::atomic<bool> warned{ false };
					WarnOnce(warned, "video rendering (CreateVideoSink)");
					VCS_MEDIA_LOG_DEBUG("[LinuxMediaFactory] CreateVideoSink ignored: %s",
						sinkId.c_str());
					return nullptr;
				}

				std::shared_ptr<IAudioSink>
					LinuxMediaFactory::CreateAudioSink(
						const std::string& sinkId,
						const vcs::media_config::AudioSinkConfig& /*config*/) {
					static std::atomic<bool> warned{ false };
					WarnOnce(warned, "audio playback (CreateAudioSink)");
					VCS_MEDIA_LOG_DEBUG("[LinuxMediaFactory] CreateAudioSink ignored: %s",
						sinkId.c_str());
					return nullptr;
				}

			}
		}

		// ============================================================
		// Platform entry（与 Windows 侧同一签名，位于 vcs::media 内）
		// ============================================================
		std::shared_ptr<IPlatformMediaFactory> CreatePlatformMediaFactory() {
			return std::make_shared<platform::linux::LinuxMediaFactory>();
		}

	} // namespace media
}
