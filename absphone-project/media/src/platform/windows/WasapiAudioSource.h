#pragma once

#include "IAudioSource.h"
#include "MediaConfig.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 前向声明 Windows 类型（不在头文件里包含 windows.h）
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioCaptureClient;
struct IAudioEndpointVolume;
struct tWAVEFORMATEX;

namespace vcs {
	namespace media {
		namespace platform {
			namespace windows {

				class WasapiAudioSource : public IAudioSource {
				public:
					// deviceIndex < 0：打开系统默认"通信"角色的采集端点
					// （媒体层 id 记为 mic:default）；>= 0：按枚举位置取端点，
					// 与系统默认设置无关，环回端点（立体声混音）也会被选中。
					explicit WasapiAudioSource(int deviceIndex);
					~WasapiAudioSource() override;

					const std::string& GetId() const override { return id_; }

					void Subscribe(const std::string& consumerId, FrameCallback cb) override;
					void Unsubscribe(const std::string& consumerId) override;

					size_t GetSubscriberCount() const override;

				private:
					void CaptureThreadFunc();
					bool InitWasapi();
					void CleanupWasapi();

					// 端点静音/音量与采集幅度诊断：共享模式采集在端点被
					// 静音、音量为 0 或 Windows 麦克风隐私拒绝桌面应用时，
					// 依然 Initialize/Start 成功，但数据全零且不一定带
					// AUDCLNT_BUFFERFLAGS_SILENT —— 只能靠幅度统计发现。
					void LogEndpointState(const char* where);
					void MaybeLogDiagnostics();

					void ProcessNativeFrames(const uint8_t* data, uint32_t numFrames, uint32_t flags);
					void ConvertToInt16Mono(const uint8_t* data, uint32_t numFrames, std::vector<int16_t>& out);
					void Resample(const std::vector<int16_t>& in, uint32_t inRate, std::vector<int16_t>& out);
					void FlushAccumulation();

					int deviceIndex_;
					std::string id_;

					uint32_t targetSampleRate_ = 8000;
					uint8_t  targetChannels_ = 1;
					uint32_t targetFrameMs_ = 20;
					uint32_t targetFrameSamples_ = 160;

					IMMDeviceEnumerator* enumerator_ = nullptr;
					IMMDevice*           device_ = nullptr;
					IAudioClient*        audioClient_ = nullptr;
					IAudioCaptureClient* captureClient_ = nullptr;
					IAudioEndpointVolume* endpointVolume_ = nullptr;
					tWAVEFORMATEX*       mixFormat_ = nullptr;

					// 端点状态（每次 1 Hz 诊断刷新）
					bool  endpointMuted_ = false;
					float endpointVolumeScalar_ = -1.0f;

					// 采集诊断（按 1 Hz 窗口统计，打日志后清零）
					uint64_t diagPackets_ = 0;
					uint64_t diagSilentFlagPackets_ = 0;
					uint64_t diagNativeFrames_ = 0;
					uint64_t diagFlushedFrames_ = 0;
					uint64_t diagZeroFrames_ = 0;
					int32_t  diagPeak8k_ = 0;
					int64_t  diagLastLogMs_ = 0;

					uint32_t nativeSampleRate_ = 0;
					uint8_t  nativeChannels_ = 0;
					uint16_t nativeBitsPerSample_ = 0;
					bool     nativeIsFloat_ = false;

					std::vector<int16_t> accumBuf_;
					std::vector<int16_t> int16MonoBuf_;
					std::vector<int16_t> resampledBuf_;

					mutable std::mutex subsMtx_;
					std::map<std::string, FrameCallback> subscribers_;

					std::thread captureThread_;
					std::atomic<bool> running_{ false };
					std::mutex startStopMtx_;
				};

			}
		}
	}
}