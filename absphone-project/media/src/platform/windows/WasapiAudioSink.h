#pragma once

#include "IAudioSink.h"
#include "MediaConfig.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 前向声明 Windows 类型
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;
struct tWAVEFORMATEX;

namespace vcs {
	namespace media {
		namespace platform {
			namespace windows {

				class WasapiAudioSink : public IAudioSink {
				public:
					explicit WasapiAudioSink(const vcs::media_config::AudioSinkConfig& config);
					~WasapiAudioSink() override;

					const std::string& GetId() const override { return id_; }

					bool BindStream(const std::string& streamId) override;
					void UnbindStream() override;
					bool Render(AudioFramePtr frame) override;
					const std::string& GetBoundStreamId() const override;

				private:
					void PlayThreadFunc();
					bool InitWasapi();
					void CleanupWasapi();

					bool ConvertFromPending(uint8_t* dst, uint32_t numFrames);

					int deviceIndex_;
					std::string id_;

					uint32_t srcSampleRate_ = 8000;
					uint8_t  srcChannels_ = 1;

					IMMDeviceEnumerator* enumerator_ = nullptr;
					IMMDevice*           device_ = nullptr;
					IAudioClient*        audioClient_ = nullptr;
					IAudioRenderClient*  renderClient_ = nullptr;
					tWAVEFORMATEX*       mixFormat_ = nullptr;

					uint32_t nativeSampleRate_ = 0;
					uint8_t  nativeChannels_ = 0;
					uint16_t nativeBitsPerSample_ = 0;
					bool     nativeIsFloat_ = false;

					std::mutex pendingMtx_;
					std::vector<int16_t> pendingBuf_;
					double   resamplePos_ = 0.0;
					static constexpr size_t kMaxPendingSamples = 8000 * 2;

					mutable std::mutex mutex_;
					std::string boundStreamId_;

					std::thread playThread_;
					std::atomic<bool> running_{ false };
					std::mutex startStopMtx_;
				};

			}
		}
	}
}