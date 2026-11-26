#include "AlsaAudioSource.h"
#include "AudioFrame.h"
#include "PlatformTime.h"
#include "ILogger.h"
#include "IMediaDebug.h"

#include <alsa/asoundlib.h>
#include <alloca.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>

namespace vcs {
	namespace media {
		namespace platform {
			namespace linux {

				namespace {

					const uint32_t kTargetRate = 8000;
					const uint32_t kTargetFrameSamples = 160;   // 20 ms @ 8 kHz
					const uint32_t kWaitTimeoutMs = 200;

					// ALSA 默认错误处理器直接写 stderr，绕过日志系统；换成 DEBUG 一行。
					// 只做诊断用：设备忙、参数不支持等细节都从这里出来。
					void AlsaErrorHandler(const char* file, int line, const char* function,
						int err, const char* fmt, ...) {
						char detail[512];
						va_list ap;
						va_start(ap, fmt);
						std::vsnprintf(detail, sizeof(detail), fmt, ap);
						va_end(ap);
						VCS_MEDIA_LOG_DEBUG("[AlsaAudioSource] alsa error: %s (%s:%d %s) err=%d",
							detail, file ? file : "-", line,
							function ? function : "-", err);
					}

					void EnsureAlsaErrorHandler() {
						static std::once_flag once;
						std::call_once(once, [] {
							snd_lib_error_set_handler(&AlsaErrorHandler);
						});
					}

					// 任意比例 box filter：G.711 语音足够，与 Windows 侧同一算法。
					void ResampleBox(const std::vector<int16_t>& in, uint32_t inRate,
						uint32_t outRate, std::vector<int16_t>& out) {
						if (in.empty() || inRate == 0 || outRate == 0) {
							out.clear();
							return;
						}
						if (inRate == outRate) {
							out = in;
							return;
						}

						const double ratio = static_cast<double>(inRate) / outRate;
						const size_t outCount = static_cast<size_t>(
							std::ceil(static_cast<double>(in.size()) / ratio));
						out.resize(outCount);

						for (size_t o = 0; o < outCount; ++o) {
							const double start = o * ratio;
							const double end = (o + 1) * ratio;
							const size_t iStart = static_cast<size_t>(start);
							size_t iEnd = static_cast<size_t>(std::ceil(end));
							if (iEnd > in.size()) iEnd = in.size();
							if (iStart >= in.size()) {
								out[o] = 0;
								continue;
							}
							if (iEnd <= iStart) {
								out[o] = in[iStart];
								continue;
							}
							int64_t sum = 0;
							for (size_t i = iStart; i < iEnd; ++i) sum += in[i];
							out[o] = static_cast<int16_t>(sum / static_cast<int64_t>(iEnd - iStart));
						}
					}

				} // anonymous namespace

				// ============================================================
				// 枚举
				//
				// 只列"真的能以采集方式打开"的 PCM：hint 里有 IOID=Output 的、
				// 播放专用的 dmix 等都会被这里过滤掉。打开是 NONBLOCK 的，不会
				// 因为设备忙卡住启动路径。
				// ============================================================
				std::vector<std::string> EnumerateAudioCaptureDevices() {
					std::vector<std::string> names;

					void** hints = nullptr;
					if (snd_device_name_hint(-1, "pcm", &hints) < 0 || !hints) {
						VCS_MEDIA_LOG_WARNING("[AlsaAudioSource] snd_device_name_hint failed");
						return names;
					}

					for (void** h = hints; *h; ++h) {
						char* name = snd_device_name_get_hint(*h, "NAME");
						char* ioid = snd_device_name_get_hint(*h, "IOID");

						if (!name) {
							free(ioid);
							continue;
						}

						const std::string pcmName(name);
						const bool maybeInput =
							(ioid == nullptr) || (std::strcmp(ioid, "Input") == 0);
						free(name);
						free(ioid);

						if (!maybeInput || pcmName.empty() || pcmName == "null") continue;
						if (std::find(names.begin(), names.end(), pcmName) != names.end()) continue;

						snd_pcm_t* probe = nullptr;
						if (snd_pcm_open(&probe, pcmName.c_str(),
							SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) == 0 && probe) {
							snd_pcm_close(probe);
							names.push_back(pcmName);
						}
					}

					snd_device_name_free_hint(hints);
					return names;
				}

				// ============================================================
				// Construction / Destruction
				// ============================================================
				AlsaAudioSource::AlsaAudioSource(std::string pcmName, std::string id)
					: pcmName_(std::move(pcmName)), id_(std::move(id)) {
					targetRate_ = kTargetRate;
					targetFrameSamples_ = kTargetFrameSamples;
					accumBuf_.reserve(targetFrameSamples_ * 4);
					VCS_MEDIA_LOG_INFO("[AlsaAudioSource] created: id=%s, pcm=%s",
						id_.c_str(), pcmName_.c_str());
				}

				AlsaAudioSource::~AlsaAudioSource() {
					running_ = false;
					if (captureThread_.joinable()) {
						captureThread_.join();
					}
					VCS_MEDIA_LOG_INFO("[AlsaAudioSource] destroyed: id=%s", id_.c_str());
				}

				// ============================================================
				// Subscribe / Unsubscribe
				// ============================================================
				void AlsaAudioSource::Subscribe(const std::string& consumerId, FrameCallback cb) {
					if (!cb) return;

					std::lock_guard<std::mutex> lock(startStopMtx_);
					{
						std::lock_guard<std::mutex> subLock(subsMtx_);
						subscribers_[consumerId] = std::move(cb);
					}

					if (!running_.load()) {
						// 上一轮线程可能因为设备打开失败自己退出了：先回收，
						// 否则给 joinable 的 thread 赋值会直接 std::terminate。
						if (captureThread_.joinable()) {
							captureThread_.join();
						}
						running_ = true;
						captureThread_ = std::thread(&AlsaAudioSource::CaptureThreadFunc, this);
						VCS_MEDIA_LOG_INFO("[AlsaAudioSource] capture thread started: id=%s, consumer=%s",
							id_.c_str(), consumerId.c_str());
					}
				}

				void AlsaAudioSource::Unsubscribe(const std::string& consumerId) {
					bool empty = false;
					{
						std::lock_guard<std::mutex> subLock(subsMtx_);
						subscribers_.erase(consumerId);
						empty = subscribers_.empty();
					}

					if (empty) {
						std::lock_guard<std::mutex> lock(startStopMtx_);
						running_ = false;
						if (captureThread_.joinable()) {
							captureThread_.join();
							VCS_MEDIA_LOG_INFO("[AlsaAudioSource] capture thread stopped: id=%s",
								id_.c_str());
						}
					}
				}

				size_t AlsaAudioSource::GetSubscriberCount() const {
					std::lock_guard<std::mutex> lock(subsMtx_);
					return subscribers_.size();
				}

				// ============================================================
				// OpenAndConfigure
				//
				// 优先按原名打开；参数（8k/mono/S16）被拒绝时退回 plug 层再试一次。
				// 采集线程内调用，失败返回 false 由调用方决定是否重试。
				// ============================================================
				bool AlsaAudioSource::OpenAndConfigure(const std::string& pcmName) {
					int rc = snd_pcm_open(&pcm_, pcmName.c_str(),
						SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
					if (rc < 0 || !pcm_) {
						pcm_ = nullptr;
						VCS_MEDIA_LOG_WARNING("[AlsaAudioSource] snd_pcm_open(%s) failed: %s",
							pcmName.c_str(), snd_strerror(rc));
						return false;
					}

					// 打开用 NONBLOCK（设备忙时不卡控制面），拿到句柄后切回阻塞，
					// 读循环靠 snd_pcm_wait 的超时来响应停止。
					snd_pcm_nonblock(pcm_, 0);

					snd_pcm_hw_params_t* params = nullptr;
					snd_pcm_hw_params_alloca(&params);

					rc = snd_pcm_hw_params_any(pcm_, params);
					if (rc >= 0) rc = snd_pcm_hw_params_set_access(pcm_, params,
						SND_PCM_ACCESS_RW_INTERLEAVED);
					if (rc >= 0) rc = snd_pcm_hw_params_set_format(pcm_, params,
						SND_PCM_FORMAT_S16_LE);
					if (rc >= 0) rc = snd_pcm_hw_params_set_channels(pcm_, params, 1);

					int dir = 0;
					unsigned int rate = kTargetRate;
					if (rc >= 0) rc = snd_pcm_hw_params_set_rate_near(pcm_, params, &rate, &dir);

					snd_pcm_uframes_t period = rate / 50;   // 20 ms
					dir = 0;
					if (rc >= 0) rc = snd_pcm_hw_params_set_period_size_near(pcm_, params, &period, &dir);

					snd_pcm_uframes_t bufferSize = period * 4;   // 80 ms
					if (rc >= 0) rc = snd_pcm_hw_params_set_buffer_size_near(pcm_, params, &bufferSize);

					if (rc >= 0) rc = snd_pcm_hw_params(pcm_, params);
					if (rc >= 0) rc = snd_pcm_prepare(pcm_);

					if (rc < 0) {
						VCS_MEDIA_LOG_WARNING(
							"[AlsaAudioSource] configure %s failed: %s (rate=%u)",
							pcmName.c_str(), snd_strerror(rc), rate);
						snd_pcm_close(pcm_);
						pcm_ = nullptr;
						return false;
					}

					// 读回设备真正接受的参数（plug 层可能已经做了转换）
					snd_pcm_hw_params_t* current = nullptr;
					snd_pcm_hw_params_alloca(&current);
					unsigned int ch = 1;
					unsigned int curRate = kTargetRate;
					snd_pcm_uframes_t curPeriod = kTargetFrameSamples;
					if (snd_pcm_hw_params_current(pcm_, current) >= 0) {
						snd_pcm_hw_params_get_channels(current, &ch);
						snd_pcm_hw_params_get_rate(current, &curRate, &dir);
						snd_pcm_hw_params_get_period_size(current, &curPeriod, &dir);
					}
					actualChannels_ = ch > 0 ? ch : 1;
					actualRate_ = curRate > 0 ? curRate : kTargetRate;
					periodFrames_ = curPeriod > 0 ? curPeriod : kTargetFrameSamples;

					VCS_MEDIA_LOG_INFO("[AlsaAudioSource] opened: id=%s, pcm=%s, %uHz/%uch, "
						"period=%llu frames, target=%uHz/%uch/%ums",
						id_.c_str(), pcmName.c_str(), actualRate_, actualChannels_,
						static_cast<unsigned long long>(periodFrames_),
						targetRate_, 1u,
						static_cast<unsigned>(targetFrameSamples_ * 1000 / targetRate_));
					return true;
				}

				bool AlsaAudioSource::Recover(int err, const char* where) {
					if (err == -EPIPE) {
						VCS_MEDIA_LOG_WARNING("[AlsaAudioSource] overrun at %s: id=%s",
							where, id_.c_str());
						snd_pcm_prepare(pcm_);
						return true;
					}
					if (err == -ESTRPIPE) {
						// 设备被挂起（例如系统休眠）：等驱动恢复，最多 1 秒
						VCS_MEDIA_LOG_WARNING("[AlsaAudioSource] device suspended at %s: id=%s",
							where, id_.c_str());
						int r = 0;
						for (int i = 0; i < 10 && (r = snd_pcm_resume(pcm_)) == -EAGAIN; ++i) {
							std::this_thread::sleep_for(std::chrono::milliseconds(100));
						}
						if (r < 0) snd_pcm_prepare(pcm_);
						return true;
					}
					if (err == -EAGAIN || err == -EINTR) return true;

					VCS_MEDIA_LOG_ERROR("[AlsaAudioSource] %s failed: id=%s, err=%s",
						where, id_.c_str(), snd_strerror(err));
					return false;
				}

				void AlsaAudioSource::CloseDevice() {
					if (pcm_) {
						snd_pcm_drop(pcm_);
						snd_pcm_close(pcm_);
						pcm_ = nullptr;
					}
					accumBuf_.clear();
					monoBuf_.clear();
					resampledBuf_.clear();
				}

				// ============================================================
				// ProcessFrames
				//
				// 原生（S16_LE，ch 声道）→ 单声道 → 8 kHz → 累积成 20 ms 帧
				// ============================================================
				void AlsaAudioSource::ProcessFrames(const int16_t* data, size_t frames) {
					if (!data || frames == 0) return;

					++diagReads_;
					diagNativeFrames_ += frames;

					monoBuf_.resize(frames);
					if (actualChannels_ <= 1) {
						std::memcpy(monoBuf_.data(), data, frames * sizeof(int16_t));
					}
					else {
						const uint32_t ch = actualChannels_;
						for (size_t i = 0; i < frames; ++i) {
							int32_t sum = 0;
							const size_t base = i * ch;
							for (uint32_t c = 0; c < ch; ++c) sum += data[base + c];
							monoBuf_[i] = static_cast<int16_t>(sum / static_cast<int32_t>(ch));
						}
					}

					ResampleBox(monoBuf_, actualRate_, targetRate_, resampledBuf_);

					for (size_t i = 0; i < resampledBuf_.size(); ++i) {
						const int32_t v = resampledBuf_[i] < 0
							? -static_cast<int32_t>(resampledBuf_[i])
							: static_cast<int32_t>(resampledBuf_[i]);
						if (v > diagPeak8k_) diagPeak8k_ = v;
					}

					accumBuf_.insert(accumBuf_.end(),
						resampledBuf_.begin(), resampledBuf_.end());

					while (accumBuf_.size() >= targetFrameSamples_) {
						FlushAccumulation();
					}
				}

				void AlsaAudioSource::FlushAccumulation() {
					if (accumBuf_.size() < targetFrameSamples_) return;

					auto frameData = std::make_shared<std::vector<int16_t>>(
						accumBuf_.begin(),
						accumBuf_.begin() + static_cast<std::ptrdiff_t>(targetFrameSamples_));
					accumBuf_.erase(accumBuf_.begin(),
						accumBuf_.begin() + static_cast<std::ptrdiff_t>(targetFrameSamples_));

					auto frame = std::make_shared<AudioFrame>();
					frame->data = frameData;
					frame->samples = targetFrameSamples_;
					frame->sampleRate = targetRate_;
					frame->channels = 1;
					frame->timestamp = vcs::base::platform::UtcNowEpochMs() * 1000;   // us

					++diagFrames_;
					bool allZero = true;
					for (size_t i = 0; i < frameData->size(); ++i) {
						if ((*frameData)[i] != 0) { allZero = false; break; }
					}
					if (allZero) ++diagZeroFrames_;

					// 与 Windows 侧同一落点：导出的是进入编码队列的 8k/1ch PCM，
					// 可与 TX 侧 tx_preencode 对照定位静音发生在哪一段。
					if (IMediaDebug* debug = GetMediaDebug()) {
						debug->SaveAudioInput(
							id_,
							reinterpret_cast<const uint8_t*>(frameData->data()),
							frameData->size() * sizeof(int16_t));
					}

					AudioFramePtr constFrame = frame;
					std::vector<FrameCallback> cbs;
					{
						std::lock_guard<std::mutex> lock(subsMtx_);
						cbs.reserve(subscribers_.size());
						for (auto& kv : subscribers_) cbs.push_back(kv.second);
					}
					for (auto& cb : cbs) {
						try {
							cb(constFrame);
						}
						catch (const std::exception& e) {
							VCS_MEDIA_LOG_ERROR("[AlsaAudioSource] subscriber threw: %s", e.what());
						}
					}
				}

				void AlsaAudioSource::MaybeLogDiagnostics() {
					const int64_t nowMs = static_cast<int64_t>(vcs::base::platform::UtcNowEpochMs());
					if (diagLastLogMs_ == 0) {
						diagLastLogMs_ = nowMs;
						return;
					}
					if (nowMs - diagLastLogMs_ < 1000) return;
					diagLastLogMs_ = nowMs;

					VCS_MEDIA_LOG_INFO(
						"[AlsaAudioSource] capture diag: id=%s, reads=%llu, nativeFrames=%llu, "
						"frames=%llu, zeroFrames=%llu, peak8k=%d, rate=%u, ch=%u",
						id_.c_str(),
						static_cast<unsigned long long>(diagReads_),
						static_cast<unsigned long long>(diagNativeFrames_),
						static_cast<unsigned long long>(diagFrames_),
						static_cast<unsigned long long>(diagZeroFrames_),
						diagPeak8k_, actualRate_, actualChannels_);

					if (diagPeak8k_ == 0 && (diagReads_ > 0 || diagFrames_ > 0)) {
						VCS_MEDIA_LOG_WARNING(
							"[AlsaAudioSource] captured audio is ALL ZERO: id=%s "
							"(check capture mute in alsamixer, or wrong device)",
							id_.c_str());
					}

					diagReads_ = 0;
					diagNativeFrames_ = 0;
					diagFrames_ = 0;
					diagZeroFrames_ = 0;
					diagPeak8k_ = 0;
				}

				// ============================================================
				// CaptureThreadFunc
				//
				// 采集线程内完成 open → 读循环 → close，与 Windows 侧同构。
				// ============================================================
				void AlsaAudioSource::CaptureThreadFunc() {
					VCS_MEDIA_LOG_INFO("[AlsaAudioSource] CaptureThreadFunc enter: id=%s",
						id_.c_str());

					EnsureAlsaErrorHandler();

					const std::string primary = pcmName_.empty() ? std::string("default") : pcmName_;
					bool opened = OpenAndConfigure(primary);
					if (!opened) {
						std::string plugged = primary;
						if (plugged.rfind("plug:", 0) != 0 && plugged.rfind("plughw:", 0) != 0) {
							plugged = "plug:" + primary;
						}
						VCS_MEDIA_LOG_WARNING(
							"[AlsaAudioSource] %s: retry with plug layer (%s)",
							id_.c_str(), plugged.c_str());
						opened = OpenAndConfigure(plugged);
					}

					if (!opened) {
						VCS_MEDIA_LOG_ERROR("[AlsaAudioSource] open failed: id=%s, pcm=%s",
							id_.c_str(), primary.c_str());
						running_ = false;
						return;
					}

					std::vector<int16_t> scratch(
						static_cast<size_t>(periodFrames_) * actualChannels_);

					while (running_.load(std::memory_order_acquire)) {
						if (!pcm_) break;

						MaybeLogDiagnostics();

						const int waitRc = snd_pcm_wait(pcm_, kWaitTimeoutMs);
						if (waitRc < 0) {
							if (!Recover(waitRc, "snd_pcm_wait")) break;
							continue;
						}
						if (waitRc == 0) continue;   // 超时：回到循环顶部检查 running_

						for (;;) {
							snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm_);
							if (avail < 0) {
								if (!Recover(static_cast<int>(avail), "snd_pcm_avail_update")) {
									running_ = false;
								}
								break;
							}
							if (avail == 0) break;

							const size_t want = static_cast<size_t>(
								std::min<snd_pcm_sframes_t>(avail,
									static_cast<snd_pcm_sframes_t>(periodFrames_)));

							const snd_pcm_sframes_t got = snd_pcm_readi(pcm_, scratch.data(), want);
							if (got < 0) {
								if (!Recover(static_cast<int>(got), "snd_pcm_readi")) {
									running_ = false;
								}
								break;
							}
							if (got == 0) break;

							ProcessFrames(scratch.data(), static_cast<size_t>(got));

							if (!running_.load()) break;
						}
					}

					CloseDevice();

					VCS_MEDIA_LOG_INFO("[AlsaAudioSource] CaptureThreadFunc exit: id=%s",
						id_.c_str());
					running_ = false;
				}

			}
		}
	}
}
