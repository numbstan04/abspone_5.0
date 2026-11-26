#include "MFVideoSource.h"
#include "ILogger.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace vcs {
	namespace media {
		namespace platform {
			namespace windows {

				MFVideoSource::MFVideoSource(int deviceIndex)
					: deviceIndex_(deviceIndex) {
					id_ = "camera:" + std::to_string(deviceIndex);
					VCS_MEDIA_LOG_INFO("[MFVideoSource] created: id=%s", id_.c_str());
				}

				MFVideoSource::~MFVideoSource() {
					// 停采集线程
					running_ = false;
					if (captureThread_.joinable()) {
						captureThread_.join();
					}
					VCS_MEDIA_LOG_INFO("[MFVideoSource] destroyed: id=%s", id_.c_str());
				}

				// ============================================================
				// Subscribe / Unsubscribe
				// ============================================================
				void MFVideoSource::Subscribe(const std::string& consumerId, FrameCallback cb) {
					if (!cb) return;

					std::lock_guard<std::mutex> lock(startStopMtx_);
					{
						std::lock_guard<std::mutex> subLock(subsMtx_);
						subscribers_[consumerId] = std::move(cb);
					}

					if (!running_.load()) {
						// 上一轮线程可能因设备打开失败自行退出：先回收，再赋新线程，
						// 否则给 joinable 的 thread 赋值会 std::terminate。
						if (captureThread_.joinable()) {
							captureThread_.join();
						}
						running_ = true;
						captureThread_ = std::thread(&MFVideoSource::CaptureThreadFunc, this);
						VCS_MEDIA_LOG_INFO("[MFVideoSource] capture thread started: id=%s, consumer=%s",
							id_.c_str(), consumerId.c_str());
					}
				}

				void MFVideoSource::Unsubscribe(const std::string& consumerId) {
					bool empty = false;
					{
						std::lock_guard<std::mutex> subLock(subsMtx_);
						subscribers_.erase(consumerId);
						empty = subscribers_.empty();
					}

					if (empty) {
						std::lock_guard<std::mutex> lock(startStopMtx_);
						running_ = false;
						// 以 joinable 而不是 running_ 为准：打开失败的线程自己会把
						// running_ 置 false 并退出，但仍然需要 join 回收。
						if (captureThread_.joinable()) {
							captureThread_.join();
							VCS_MEDIA_LOG_INFO("[MFVideoSource] capture thread stopped: id=%s",
								id_.c_str());
						}
					}
				}

				size_t MFVideoSource::GetSubscriberCount() const {
					std::lock_guard<std::mutex> lock(subsMtx_);
					return subscribers_.size();
				}

				// ============================================================
				// OpenDevice / CloseDevice
				// ============================================================
				bool MFVideoSource::OpenDevice() {
					// 1. 枚举摄像头
					IMFAttributes* attr = nullptr;
					HRESULT hr = MFCreateAttributes(&attr, 1);
					if (FAILED(hr)) return false;

					hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
						MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
					if (FAILED(hr)) {
						attr->Release();
						return false;
					}

					IMFActivate** devices = nullptr;
					UINT32 count = 0;
					hr = MFEnumDeviceSources(attr, &devices, &count);
					attr->Release();

					if (FAILED(hr) || static_cast<UINT32>(deviceIndex_) >= count) {
						if (devices) CoTaskMemFree(devices);
						VCS_MEDIA_LOG_ERROR("[MFVideoSource] camera index=%d not found (count=%u)",
							deviceIndex_, count);
						return false;
					}

					// 2. 激活指定摄像头
					hr = devices[deviceIndex_]->ActivateObject(__uuidof(IMFMediaSource),
						(void**)&mediaSource_);
					for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
					CoTaskMemFree(devices);

					if (FAILED(hr) || !mediaSource_) {
						VCS_MEDIA_LOG_ERROR("[MFVideoSource] ActivateObject failed: 0x%08X",
							static_cast<unsigned>(hr));
						return false;
					}

					// 3. 创建 SourceReader，启用高级视频处理（自动颜色转换）
					IMFAttributes* readerAttr = nullptr;
					MFCreateAttributes(&readerAttr, 2);
					if (readerAttr) {
						readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
						readerAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
					}

					hr = MFCreateSourceReaderFromMediaSource(mediaSource_, readerAttr, &reader_);
					if (readerAttr) readerAttr->Release();
					if (FAILED(hr) || !reader_) {
						VCS_MEDIA_LOG_ERROR("[MFVideoSource] MFCreateSourceReaderFromMediaSource failed: 0x%08X",
							static_cast<unsigned>(hr));
						CloseDevice();
						return false;
					}

					// 4. 请求输出 YUV420P
					IMFMediaType* outType = nullptr;
					hr = MFCreateMediaType(&outType);
					if (FAILED(hr)) { CloseDevice(); return false; }

					outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
					outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV);
					MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, 640, 360);
					hr = reader_->SetCurrentMediaType(
						static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
						nullptr, outType);
					outType->Release();

					if (FAILED(hr)) {
						VCS_MEDIA_LOG_ERROR("[MFVideoSource] SetCurrentMediaType(YUV420P) failed: 0x%08X "
							"(driver may not support YUV420P output)",
							static_cast<unsigned>(hr));
						CloseDevice();
						return false;
					}

					// 5. 查询实际输出的分辨率
					IMFMediaType* curType = nullptr;
					if (SUCCEEDED(reader_->GetCurrentMediaType(
						static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
						&curType)) && curType) {
						MFGetAttributeSize(curType, MF_MT_FRAME_SIZE, &width_, &height_);
						curType->Release();
					}

					if (width_ == 0 || height_ == 0) {
						VCS_MEDIA_LOG_ERROR("[MFVideoSource] cannot query frame size");
						CloseDevice();
						return false;
					}

					VCS_MEDIA_LOG_INFO("[MFVideoSource] opened: id=%s, %ux%u",
						id_.c_str(), width_, height_);
					return true;
				}

				void MFVideoSource::CloseDevice() {
					if (reader_) {
						reader_->Release();
						reader_ = nullptr;
					}
					if (mediaSource_) {
						mediaSource_->Shutdown();
						mediaSource_->Release();
						mediaSource_ = nullptr;
					}
					width_ = 0;
					height_ = 0;
				}

				// ============================================================
				// CaptureThreadFunc
				//
				// 采集线程内完成：COM 初始化 → OpenDevice → ReadSample 循环 → CloseDevice → COM 反初始化
				// ============================================================
				void MFVideoSource::CaptureThreadFunc() {
					// 采集线程单独 COM 初始化（MTA）
					HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
					const bool comOk = SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE;

					VCS_MEDIA_LOG_INFO("[MFVideoSource] CaptureThreadFunc enter: id=%s", id_.c_str());

					if (!OpenDevice()) {
						VCS_MEDIA_LOG_ERROR("[MFVideoSource] OpenDevice failed: id=%s", id_.c_str());
						running_ = false;
						if (comOk && SUCCEEDED(comHr)) CoUninitialize();
						return;
					}

					while (running_.load(std::memory_order_acquire)) {
						if (!reader_) break;

						DWORD actualStreamIndex = 0;
						DWORD streamFlags = 0;
						LONGLONG timestamp = 0;
						IMFSample* sample = nullptr;

						HRESULT hr = reader_->ReadSample(
							static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
							0, &actualStreamIndex, &streamFlags, &timestamp, &sample);

						if (FAILED(hr)) {
							if (running_.load()) {
								VCS_MEDIA_LOG_WARNING("[MFVideoSource] ReadSample failed: 0x%08X",
									static_cast<unsigned>(hr));
								std::this_thread::sleep_for(std::chrono::milliseconds(50));
							}
							continue;
						}

						if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
							VCS_MEDIA_LOG_INFO("[MFVideoSource] end of stream: id=%s", id_.c_str());
							break;
						}
						if (!sample) continue;   // 暂时无帧

						// 从 sample 取 buffer
						IMFMediaBuffer* mediaBuffer = nullptr;
						hr = sample->ConvertToContiguousBuffer(&mediaBuffer);
						if (FAILED(hr) || !mediaBuffer) {
							sample->Release();
							continue;
						}

						BYTE* data = nullptr;
						DWORD maxLen = 0;
						DWORD curLen = 0;
						hr = mediaBuffer->Lock(&data, &maxLen, &curLen);

						if (SUCCEEDED(hr) && data && curLen > 0 && width_ > 0 && height_ > 0) {
							const size_t ySize = static_cast<size_t>(width_) * height_;
							const size_t uvSize = ySize / 4;

							// 查询当前媒体的 stride（YUV420P 通常是 width，但可能有 padding）
							LONG stride = static_cast<LONG>(width_);
							IMFMediaType* curType = nullptr;
							if (SUCCEEDED(reader_->GetCurrentMediaType(
								static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
								&curType)) && curType) {
								UINT32 s = 0;
								if (SUCCEEDED(curType->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) && s != 0) {
									stride = static_cast<LONG>(static_cast<INT32>(s));
									if (stride < 0) stride = -stride;
								}
								curType->Release();
							}

							// 检查 buffer 大小是否够
							const size_t needed = static_cast<size_t>(stride) * height_ * 3 / 2;
							if (curLen >= needed) {
								auto frameData = std::make_shared<std::vector<uint8_t>>();
								frameData->resize(ySize + uvSize * 2);

								// ---- Y plane ----
								for (uint32_t row = 0; row < height_; ++row) {
									std::memcpy(frameData->data() + static_cast<size_t>(row) * width_,
										data + static_cast<size_t>(row) * stride,
										width_);
								}

								// ---- U plane ----
								const uint8_t* uSrc = data + static_cast<size_t>(stride) * height_;
								const size_t uvStride = static_cast<size_t>(stride) / 2;
								for (uint32_t row = 0; row < height_ / 2; ++row) {
									std::memcpy(frameData->data() + ySize + static_cast<size_t>(row) * (width_ / 2),
										uSrc + static_cast<size_t>(row) * uvStride,
										width_ / 2);
								}

								// ---- V plane ----
								const uint8_t* vSrc = uSrc + uvStride * (height_ / 2);
								for (uint32_t row = 0; row < height_ / 2; ++row) {
									std::memcpy(frameData->data() + ySize + uvSize + static_cast<size_t>(row) * (width_ / 2),
										vSrc + static_cast<size_t>(row) * uvStride,
										width_ / 2);
								}

								// ---- 构造 VideoFrame ----
								auto frame = std::make_shared<VideoFrame>();
								frame->data = frameData;
								frame->dataSize = frameData->size();
								frame->width = width_;
								frame->height = height_;
								frame->timestamp = static_cast<uint64_t>(timestamp) / 10;  // 100ns -> us
								frame->pixelFormat = VideoFrame::PixelFormat::YUV420P;
								frame->memberId = id_;

								// ---- 广播给订阅者（锁外） ----
								std::shared_ptr<const VideoFrame> constFrame = frame;
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
										VCS_MEDIA_LOG_ERROR("[MFVideoSource] subscriber threw: %s",
											e.what());
									}
								}

								VCS_MEDIA_LOG_TRACE("[MFVideoSource] Captured 1 frame, size=%zu", frame->dataSize);
							}
							else {
								VCS_MEDIA_LOG_DEBUG("[MFVideoSource] frame too small: curLen=%u, "
									"needed=%zu, stride=%ld",
									curLen, needed, stride);
							}

							mediaBuffer->Unlock();
						}

						mediaBuffer->Release();
						sample->Release();
					}

					CloseDevice();

					if (comOk && SUCCEEDED(comHr)) {
						CoUninitialize();
					}

					VCS_MEDIA_LOG_INFO("[MFVideoSource] CaptureThreadFunc exit: id=%s", id_.c_str());
					running_ = false;
				}

			}
		}
	}
}