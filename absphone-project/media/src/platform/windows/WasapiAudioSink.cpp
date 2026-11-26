#include "WasapiAudioSink.h"
#include "AudioFrame.h"
#include "ILogger.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>

#pragma comment(lib, "ole32.lib")

namespace vcs {
	namespace media {
		namespace platform {
			namespace windows {

				// 极简 COM 智能指针（放在这里防止外部找不到）
				template <typename T>
				class ComPtr {
				public:
					ComPtr() = default;
					~ComPtr() { Reset(); }
					ComPtr(const ComPtr&) = delete;
					ComPtr& operator=(const ComPtr&) = delete;

					T** Put() { Reset(); return &p_; }
					T*  Get() const { return p_; }
					T*  operator->() const { return p_; }
					explicit operator bool() const { return p_ != nullptr; }
					void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }

				private:
					T* p_ = nullptr;
				};

				namespace {
					bool IsFloatFormat(const WAVEFORMATEX* fmt) {
						if (!fmt) return false;
						if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
						if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
							const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
							return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
						}
						return false;
					}
				} // anonymous namespace

				// ============================================================
				// Construction / Destruction
				// ============================================================
				WasapiAudioSink::WasapiAudioSink(const vcs::media_config::AudioSinkConfig& config)
					: deviceIndex_(-1) {
					if (!config.deviceId.empty() && config.deviceId.rfind("speaker:", 0) == 0) {
						deviceIndex_ = std::atoi(config.deviceId.c_str() + 8);
						id_ = config.deviceId;
					}
					else {
						id_ = "speaker:default";
					}
					pendingBuf_.reserve(kMaxPendingSamples);
					VCS_MEDIA_LOG_INFO("[WasapiAudioSink] created: id=%s, deviceIndex=%d", id_.c_str(), deviceIndex_);
				}

				WasapiAudioSink::~WasapiAudioSink() {
					running_ = false;
					if (playThread_.joinable()) {
						playThread_.join();
					}
					VCS_MEDIA_LOG_INFO("[WasapiAudioSink] destroyed");
				}

				// ============================================================
				// BindStream / UnbindStream
				// ============================================================
				bool WasapiAudioSink::BindStream(const std::string& streamId) {
					std::lock_guard<std::mutex> lock(mutex_);
					if (!boundStreamId_.empty() && boundStreamId_ != streamId) {
						return false;
					}
					boundStreamId_ = streamId;

					std::lock_guard<std::mutex> ssLock(startStopMtx_);
					if (!running_.load()) {
						// 上一轮线程可能因初始化失败自行退出：先回收再赋新线程，
						// 否则给 joinable 的 thread 赋值会 std::terminate。
						if (playThread_.joinable()) {
							playThread_.join();
						}
						running_ = true;
						playThread_ = std::thread(&WasapiAudioSink::PlayThreadFunc, this);
					}
					return true;
				}

				void WasapiAudioSink::UnbindStream() {
					{
						std::lock_guard<std::mutex> lock(mutex_);
						if (boundStreamId_.empty()) return;
						boundStreamId_.clear();
					}

					std::lock_guard<std::mutex> ssLock(startStopMtx_);
					running_ = false;
					// 以 joinable 而不是 running_ 为准：初始化失败的线程自己会把
					// running_ 置 false 并退出，但仍然需要 join 回收。
					if (playThread_.joinable()) {
						playThread_.join();
					}
				}

				const std::string& WasapiAudioSink::GetBoundStreamId() const {
					std::lock_guard<std::mutex> lock(mutex_);
					return boundStreamId_;
				}

				// ============================================================
				// Render
				// ============================================================
				bool WasapiAudioSink::Render(AudioFramePtr frame) {
					if (!frame || !frame->data || frame->samples == 0) return false;

					{
						std::lock_guard<std::mutex> lock(mutex_);
						if (!boundStreamId_.empty()) {
							// 校验逻辑
						}
					}

					if (frame->sampleRate != srcSampleRate_ || frame->channels != srcChannels_) {
						return false;
					}

					{
						std::lock_guard<std::mutex> lock(pendingMtx_);
						if (pendingBuf_.size() + frame->samples > kMaxPendingSamples) {
							const size_t drop = pendingBuf_.size() + frame->samples - kMaxPendingSamples;
							const size_t dropClamped = (std::min)(drop, pendingBuf_.size());
							pendingBuf_.erase(pendingBuf_.begin(), pendingBuf_.begin() + static_cast<std::ptrdiff_t>(dropClamped));
							resamplePos_ = 0.0;
						}
						pendingBuf_.insert(pendingBuf_.end(), frame->data->begin(), frame->data->end());
					}

					return true;
				} // <--- 确保这里有闭合括号

				// ============================================================
				// InitWasapi
				// ============================================================
				bool WasapiAudioSink::InitWasapi() {
					ComPtr<IMMDeviceEnumerator> enumTmp;
					HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
						__uuidof(IMMDeviceEnumerator), (void**)enumTmp.Put());
					if (FAILED(hr)) return false;
					enumerator_ = enumTmp.Get();
					enumerator_->AddRef();

					ComPtr<IMMDevice> devTmp;
					if (deviceIndex_ < 0) {
						hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, devTmp.Put());
					}
					else {
						ComPtr<IMMDeviceCollection> collection;
						hr = enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.Put());
						if (FAILED(hr)) {
							CleanupWasapi();
							return false;
						}
						UINT count = 0;
						collection->GetCount(&count);
						if (static_cast<UINT>(deviceIndex_) >= count) {
							CleanupWasapi();
							return false;
						}
						hr = collection->Item(static_cast<UINT>(deviceIndex_), devTmp.Put());
					}

					if (FAILED(hr)) {
						CleanupWasapi();
						return false;
					}
					device_ = devTmp.Get();
					device_->AddRef();

					ComPtr<IAudioClient> clientTmp;
					hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)clientTmp.Put());
					if (FAILED(hr)) {
						CleanupWasapi();
						return false;
					}
					audioClient_ = clientTmp.Get();
					audioClient_->AddRef();

					WAVEFORMATEX* mixFmt = nullptr;
					hr = audioClient_->GetMixFormat(&mixFmt);
					if (FAILED(hr) || !mixFmt) {
						CleanupWasapi();
						return false;
					}
					mixFormat_ = mixFmt;

					nativeSampleRate_ = mixFormat_->nSamplesPerSec;
					nativeChannels_ = static_cast<uint8_t>(mixFormat_->nChannels);
					nativeBitsPerSample_ = mixFormat_->wBitsPerSample;
					nativeIsFloat_ = IsFloatFormat(mixFormat_);

					const REFERENCE_TIME bufferDuration = 100 * 10000;
					hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, mixFormat_, nullptr);
					if (FAILED(hr)) {
						CleanupWasapi();
						return false;
					}

					ComPtr<IAudioRenderClient> rcTmp;
					hr = audioClient_->GetService(__uuidof(IAudioRenderClient), (void**)rcTmp.Put());
					if (FAILED(hr)) {
						CleanupWasapi();
						return false;
					}
					renderClient_ = rcTmp.Get();
					renderClient_->AddRef();

					hr = audioClient_->Start();
					if (FAILED(hr)) {
						CleanupWasapi();
						return false;
					}
					return true;
				}

				void WasapiAudioSink::CleanupWasapi() {
					if (audioClient_) audioClient_->Stop();
					if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
					if (audioClient_) { audioClient_->Release();  audioClient_ = nullptr; }
					if (device_) { device_->Release();       device_ = nullptr; }
					if (enumerator_) { enumerator_->Release();   enumerator_ = nullptr; }
					if (mixFormat_) {
						CoTaskMemFree(mixFormat_);
						mixFormat_ = nullptr;
					}
				}

				// ============================================================
				// ConvertFromPending
				// ============================================================
				bool WasapiAudioSink::ConvertFromPending(uint8_t* dst, uint32_t numFrames) {
					if (!dst || numFrames == 0) return false;
					if (nativeSampleRate_ == 0 || nativeChannels_ == 0) return false;

					std::vector<int16_t> localBuf;
					{
						std::lock_guard<std::mutex> lock(pendingMtx_);
						if (pendingBuf_.empty()) return false;
						localBuf.swap(pendingBuf_);
					}

					const double srcRate = static_cast<double>(srcSampleRate_);
					const double dstRate = static_cast<double>(nativeSampleRate_);
					const double step = srcRate / dstRate;

					const double maxPos = resamplePos_ + (numFrames - 1) * step;
					if (maxPos >= static_cast<double>(localBuf.size())) {
						const size_t consumed = static_cast<size_t>(std::floor(resamplePos_));
						const size_t consumedClamped = (std::min)(consumed, localBuf.size());

						std::lock_guard<std::mutex> lock(pendingMtx_);
						pendingBuf_.assign(localBuf.begin() + static_cast<std::ptrdiff_t>(consumedClamped), localBuf.end());
						resamplePos_ -= static_cast<double>(consumedClamped);
						return false;
					}

					const size_t srcCount = localBuf.size();
					for (UINT32 i = 0; i < numFrames; ++i) {
						const double pos = resamplePos_ + i * step;
						const size_t idx = static_cast<size_t>(pos);
						const double frac = pos - static_cast<double>(idx);

						int16_t s0 = localBuf[idx];
						int16_t s1 = (idx + 1 < srcCount) ? localBuf[idx + 1] : s0;
						const double interp = s0 * (1.0 - frac) + s1 * frac;

						for (uint8_t c = 0; c < nativeChannels_; ++c) {
							const size_t offset = static_cast<size_t>(i) * nativeChannels_ + c;
							if (nativeIsFloat_) {
								float* fDst = reinterpret_cast<float*>(dst);
								fDst[offset] = static_cast<float>(interp / 32768.0);
							}
							else {
								int16_t* iDst = reinterpret_cast<int16_t*>(dst);
								iDst[offset] = static_cast<int16_t>(interp);
							}
						}
					}

					resamplePos_ += numFrames * step;
					const size_t consumed = static_cast<size_t>(std::floor(resamplePos_));
					const size_t consumedClamped = (std::min)(consumed, srcCount);

					{
						std::lock_guard<std::mutex> lock(pendingMtx_);
						pendingBuf_.assign(localBuf.begin() + static_cast<std::ptrdiff_t>(consumedClamped), localBuf.end());
						resamplePos_ -= static_cast<double>(consumedClamped);
					}
					return true;
				}

				// ============================================================
				// PlayThreadFunc
				// ============================================================
				void WasapiAudioSink::PlayThreadFunc() {
					HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
					const bool comOk = SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE;

					if (!InitWasapi()) {
						running_ = false;
						if (comOk && SUCCEEDED(comHr)) CoUninitialize();
						return;
					}

					UINT32 bufferFrames = 0;
					if (FAILED(audioClient_->GetBufferSize(&bufferFrames)) || bufferFrames == 0) {
						CleanupWasapi();
						running_ = false;
						if (comOk && SUCCEEDED(comHr)) CoUninitialize();
						return;
					}

					while (running_.load(std::memory_order_acquire)) {
						UINT32 padding = 0;
						if (FAILED(audioClient_->GetCurrentPadding(&padding))) {
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
							continue;
						}

						const UINT32 available = bufferFrames - padding;
						if (available == 0) {
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
							continue;
						}

						BYTE* data = nullptr;
						if (FAILED(renderClient_->GetBuffer(available, &data)) || !data) {
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
							continue;
						}

						if (!ConvertFromPending(data, available)) {
							renderClient_->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT);
						}
						else {
							renderClient_->ReleaseBuffer(available, 0);
						}
					}

					CleanupWasapi();
					if (comOk && SUCCEEDED(comHr)) CoUninitialize();
					running_ = false;
				}

			}
		}
	}
}