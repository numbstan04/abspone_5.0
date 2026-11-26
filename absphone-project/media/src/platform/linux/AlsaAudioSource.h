#pragma once

#include "IAudioSource.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 前向声明，避免在头文件里拉 ALSA 头
struct _snd_pcm;
typedef struct _snd_pcm snd_pcm_t;

namespace vcs {
	namespace media {
		namespace platform {
			namespace linux {

				/**
				 * @brief ALSA 麦克风采集
				 *
				 * 输出固定为 8 kHz / 单声道 / 20 ms 一帧，与 Windows 的
				 * WasapiAudioSource 完全一致（AudioProcessor 与 G.711 的上游契约）。
				 *
				 * 线程模型与 Windows 侧相同：
				 *   - 第一个 Subscribe 打开设备并启动采集线程
				 *   - 最后一个 Unsubscribe 停止并 join
				 *   - 回调在采集线程执行，订阅者必须快速返回
				 *
				 * 设备在采集线程内打开/关闭，Subscribe 不会因为设备忙而阻塞控制面；
				 * 打开失败只记 ERROR，线程自行退出（上层走"无采集"路径）。
				 */
				class AlsaAudioSource : public IAudioSource {
				public:
					// pcmName 为 ALSA PCM 名（"default" / 枚举得到的名字）；
					// id 为媒体层设备 id（"mic:default" / "mic:N"）。
					AlsaAudioSource(std::string pcmName, std::string id);
					~AlsaAudioSource() override;

					const std::string& GetId() const override { return id_; }

					void Subscribe(const std::string& consumerId, FrameCallback cb) override;
					void Unsubscribe(const std::string& consumerId) override;

					size_t GetSubscriberCount() const override;

				private:
					void CaptureThreadFunc();
					bool OpenAndConfigure(const std::string& pcmName);
					void CloseDevice();
					bool Recover(int err, const char* where);

					void ProcessFrames(const int16_t* data, size_t frames);
					void FlushAccumulation();
					void MaybeLogDiagnostics();

					std::string pcmName_;
					std::string id_;

					snd_pcm_t* pcm_ = nullptr;
					uint32_t actualRate_ = 8000;
					uint32_t actualChannels_ = 1;
					uint64_t periodFrames_ = 160;

					uint32_t targetRate_ = 8000;
					uint32_t targetFrameSamples_ = 160;

					std::vector<int16_t> monoBuf_;
					std::vector<int16_t> resampledBuf_;
					std::vector<int16_t> accumBuf_;

					// 1 Hz 采集诊断（打日志后清零）
					uint64_t diagReads_ = 0;
					uint64_t diagNativeFrames_ = 0;
					uint64_t diagFrames_ = 0;
					uint64_t diagZeroFrames_ = 0;
					int32_t  diagPeak8k_ = 0;
					int64_t  diagLastLogMs_ = 0;

					mutable std::mutex subsMtx_;
					std::map<std::string, FrameCallback> subscribers_;

					std::thread captureThread_;
					std::atomic<bool> running_{ false };
					std::mutex startStopMtx_;
				};

				// 枚举可用的采集 PCM 名（顺序即 mic:N 的 N），探测失败的不列出。
				std::vector<std::string> EnumerateAudioCaptureDevices();

			}
		}
	}
}
