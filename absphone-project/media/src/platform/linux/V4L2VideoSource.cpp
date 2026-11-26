#include "V4L2VideoSource.h"
#include "PlatformTime.h"
#include "ILogger.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>

namespace vcs {
	namespace media {
		namespace platform {
			namespace linux {

				namespace {

					const int kMaxVideoNodes = 64;
					const uint32_t kRequestWidth = 640;
					const uint32_t kRequestHeight = 360;
					const uint32_t kRequestFps = 30;
					const int kPollTimeoutMs = 200;

					const char* PixelFormatName(uint32_t fmt) {
						switch (fmt) {
						case V4L2_PIX_FMT_YUYV:   return "YUYV";
						case V4L2_PIX_FMT_NV12:   return "NV12";
						case V4L2_PIX_FMT_YUV420: return "I420";
						case V4L2_PIX_FMT_YV12:   return "YV12";
						default:                  return "unknown";
						}
					}

					// YUYV（packed 4:2:2）→ I420：Y 逐点抽取，色度按 2x2 平均。
					bool YuyvToI420(const uint8_t* src, size_t size, uint32_t w, uint32_t h,
						size_t srcStride, std::vector<uint8_t>& dst) {
						const size_t ySize = static_cast<size_t>(w) * h;
						const size_t cSize = ySize / 4;
						if (size < (h - 1) * srcStride + static_cast<size_t>(w) * 2) return false;

						dst.resize(ySize + cSize * 2);
						uint8_t* dstY = dst.data();
						uint8_t* dstU = dstY + ySize;
						uint8_t* dstV = dstU + cSize;

						for (uint32_t y = 0; y < h; ++y) {
							const uint8_t* row = src + static_cast<size_t>(y) * srcStride;
							uint8_t* out = dstY + static_cast<size_t>(y) * w;
							for (uint32_t x = 0; x < w; ++x) {
								out[x] = row[x * 2];
							}
						}

						for (uint32_t y = 0; y + 1 < h; y += 2) {
							const uint8_t* r0 = src + static_cast<size_t>(y) * srcStride;
							const uint8_t* r1 = src + static_cast<size_t>(y + 1) * srcStride;
							uint8_t* du = dstU + static_cast<size_t>(y / 2) * (w / 2);
							uint8_t* dv = dstV + static_cast<size_t>(y / 2) * (w / 2);
							for (uint32_t x = 0; x + 1 < w; x += 2) {
								const unsigned u = r0[x * 2 + 1] + r1[x * 2 + 1];
								const unsigned v = r0[x * 2 + 3] + r1[x * 2 + 3];
								du[x / 2] = static_cast<uint8_t>((u + 1) / 2);
								dv[x / 2] = static_cast<uint8_t>((v + 1) / 2);
							}
						}
						return true;
					}

					// NV12 → I420：Y 直接搬，UV 交错平面拆成 U/V 两个平面。
					bool Nv12ToI420(const uint8_t* src, size_t size, uint32_t w, uint32_t h,
						size_t srcStride, std::vector<uint8_t>& dst) {
						const size_t ySize = static_cast<size_t>(w) * h;
						const size_t cSize = ySize / 4;
						const size_t uvOffset = srcStride * h;
						if (size < uvOffset + static_cast<size_t>(h / 2 - 1) * srcStride + w) return false;

						dst.resize(ySize + cSize * 2);
						uint8_t* dstY = dst.data();
						uint8_t* dstU = dstY + ySize;
						uint8_t* dstV = dstU + cSize;

						for (uint32_t y = 0; y < h; ++y) {
							std::memcpy(dstY + static_cast<size_t>(y) * w,
								src + static_cast<size_t>(y) * srcStride, w);
						}
						const uint8_t* uv = src + uvOffset;
						for (uint32_t y = 0; y + 1 < h; y += 2) {
							const uint8_t* row = uv + static_cast<size_t>(y / 2) * srcStride;
							uint8_t* du = dstU + static_cast<size_t>(y / 2) * (w / 2);
							uint8_t* dv = dstV + static_cast<size_t>(y / 2) * (w / 2);
							for (uint32_t x = 0; x + 1 < w; x += 2) {
								du[x / 2] = row[x];
								dv[x / 2] = row[x + 1];
							}
						}
						return true;
					}

					// 平面 YUV420（I420 / YV12）→ I420：逐平面搬，stride 可能大于宽度。
					bool PlanarToI420(const uint8_t* src, size_t size, uint32_t w, uint32_t h,
						size_t srcStride, bool swapUv, std::vector<uint8_t>& dst) {
						const size_t ySize = static_cast<size_t>(w) * h;
						const size_t cSize = ySize / 4;
						const size_t cStride = srcStride / 2;
						const size_t plane1 = srcStride * h;
						const size_t plane2 = plane1 + cStride * (h / 2);
						if (size < plane2 + static_cast<size_t>(h / 2 - 1) * cStride + w / 2) return false;

						dst.resize(ySize + cSize * 2);
						uint8_t* dstY = dst.data();
						uint8_t* dstU = dstY + ySize;
						uint8_t* dstV = dstU + cSize;

						for (uint32_t y = 0; y < h; ++y) {
							std::memcpy(dstY + static_cast<size_t>(y) * w,
								src + static_cast<size_t>(y) * srcStride, w);
						}
						const uint8_t* srcU = swapUv ? src + plane2 : src + plane1;
						const uint8_t* srcV = swapUv ? src + plane1 : src + plane2;
						for (uint32_t y = 0; y < h / 2; ++y) {
							std::memcpy(dstU + static_cast<size_t>(y) * (w / 2),
								srcU + static_cast<size_t>(y) * cStride, w / 2);
							std::memcpy(dstV + static_cast<size_t>(y) * (w / 2),
								srcV + static_cast<size_t>(y) * cStride, w / 2);
						}
						return true;
					}

				} // anonymous namespace

				// ============================================================
				// 枚举
				// ============================================================
				std::vector<VideoSourceInfo> EnumerateVideoCaptureDevices() {
					std::vector<VideoSourceInfo> result;

					for (int i = 0; i < kMaxVideoNodes; ++i) {
						const std::string path = "/dev/video" + std::to_string(i);
						const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
						if (fd < 0) continue;

						v4l2_capability cap;
						std::memset(&cap, 0, sizeof(cap));
						if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
							// 多节点设备（例如 UVC 的控制节点）只有 capture 位才算采集源
							const uint32_t caps = cap.device_caps ? cap.device_caps : cap.capabilities;
							const bool capture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0;
							const bool streaming = (caps & V4L2_CAP_STREAMING) != 0;
							if (capture && streaming) {
								VideoSourceInfo info;
								info.id = "camera:" + std::to_string(i);
								info.type = VideoSourceType::Camera;
								info.displayName = std::string(
									reinterpret_cast<const char*>(cap.card));
								result.push_back(std::move(info));
							}
						}
						::close(fd);
					}

					VCS_MEDIA_LOG_INFO("[V4L2VideoSource] EnumerateVideoSources: %zu",
						result.size());
					return result;
				}

				// ============================================================
				// Construction / Destruction
				// ============================================================
				V4L2VideoSource::V4L2VideoSource(int deviceIndex)
					: deviceIndex_(deviceIndex) {
					id_ = "camera:" + std::to_string(deviceIndex);
					VCS_MEDIA_LOG_INFO("[V4L2VideoSource] created: id=%s", id_.c_str());
				}

				V4L2VideoSource::~V4L2VideoSource() {
					running_ = false;
					if (captureThread_.joinable()) {
						captureThread_.join();
					}
					VCS_MEDIA_LOG_INFO("[V4L2VideoSource] destroyed: id=%s", id_.c_str());
				}

				// ============================================================
				// Subscribe / Unsubscribe
				// ============================================================
				void V4L2VideoSource::Subscribe(const std::string& consumerId, FrameCallback cb) {
					if (!cb) return;

					std::lock_guard<std::mutex> lock(startStopMtx_);
					{
						std::lock_guard<std::mutex> subLock(subsMtx_);
						subscribers_[consumerId] = std::move(cb);
					}

					if (!running_.load()) {
						// 上一轮线程可能因设备打开失败自行退出，先回收再赋新线程
						if (captureThread_.joinable()) {
							captureThread_.join();
						}
						running_ = true;
						captureThread_ = std::thread(&V4L2VideoSource::CaptureThreadFunc, this);
						VCS_MEDIA_LOG_INFO("[V4L2VideoSource] capture thread started: id=%s, consumer=%s",
							id_.c_str(), consumerId.c_str());
					}
				}

				void V4L2VideoSource::Unsubscribe(const std::string& consumerId) {
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
							VCS_MEDIA_LOG_INFO("[V4L2VideoSource] capture thread stopped: id=%s",
								id_.c_str());
						}
					}
				}

				size_t V4L2VideoSource::GetSubscriberCount() const {
					std::lock_guard<std::mutex> lock(subsMtx_);
					return subscribers_.size();
				}

				void V4L2VideoSource::LogSupportedFormats() {
					for (uint32_t index = 0;; ++index) {
						v4l2_fmtdesc desc;
						std::memset(&desc, 0, sizeof(desc));
						desc.index = index;
						desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
						if (ioctl(fd_, VIDIOC_ENUM_FMT, &desc) < 0) break;
						VCS_MEDIA_LOG_INFO("[V4L2VideoSource] supported format: %s",
							reinterpret_cast<const char*>(desc.description));
					}
				}

				bool V4L2VideoSource::SetFormat() {
					static const uint32_t kCandidates[] = {
						V4L2_PIX_FMT_YUYV,
						V4L2_PIX_FMT_NV12,
						V4L2_PIX_FMT_YUV420,
						V4L2_PIX_FMT_YV12,
					};

					for (uint32_t fmt : kCandidates) {
						v4l2_format f;
						std::memset(&f, 0, sizeof(f));
						f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
						f.fmt.pix.width = kRequestWidth;
						f.fmt.pix.height = kRequestHeight;
						f.fmt.pix.pixelformat = fmt;
						f.fmt.pix.field = V4L2_FIELD_ANY;

						if (ioctl(fd_, VIDIOC_S_FMT, &f) < 0) continue;
						// 驱动可能"接受"但换成别的格式，必须回读确认
						if (f.fmt.pix.pixelformat != fmt) continue;
						if (f.fmt.pix.width == 0 || f.fmt.pix.height == 0) continue;
						// I420 要求偶数宽高；奇数帧直接丢弃比输出错位画面好
						if ((f.fmt.pix.width & 1u) || (f.fmt.pix.height & 1u)) continue;

						width_ = f.fmt.pix.width;
						height_ = f.fmt.pix.height;
						pixelFormat_ = fmt;
						const uint32_t defaultStride =
							(fmt == V4L2_PIX_FMT_YUYV) ? width_ * 2 : width_;
						stride_ = f.fmt.pix.bytesperline ? f.fmt.pix.bytesperline : defaultStride;
						if (stride_ < defaultStride) stride_ = defaultStride;

						if (width_ != kRequestWidth || height_ != kRequestHeight) {
							VCS_MEDIA_LOG_INFO(
								"[V4L2VideoSource] requested %ux%u, driver adjusted to %ux%u",
								kRequestWidth, kRequestHeight, width_, height_);
						}
						return true;
					}

					VCS_MEDIA_LOG_ERROR(
						"[V4L2VideoSource] no supported raw format (YUYV/NV12/I420/YV12) on %s",
						id_.c_str());
					LogSupportedFormats();
					return false;
				}

				bool V4L2VideoSource::StartStreaming() {
					v4l2_requestbuffers req;
					std::memset(&req, 0, sizeof(req));
					req.count = 4;
					req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					req.memory = V4L2_MEMORY_MMAP;

					if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
						VCS_MEDIA_LOG_ERROR("[V4L2VideoSource] VIDIOC_REQBUFS failed: %s",
							std::strerror(errno));
						return false;
					}

					buffers_.resize(req.count);
					for (uint32_t i = 0; i < req.count; ++i) {
						v4l2_buffer buf;
						std::memset(&buf, 0, sizeof(buf));
						buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
						buf.memory = V4L2_MEMORY_MMAP;
						buf.index = i;
						if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
							VCS_MEDIA_LOG_ERROR("[V4L2VideoSource] VIDIOC_QUERYBUF(%u) failed: %s",
								i, std::strerror(errno));
							return false;
						}

						void* start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
							MAP_SHARED, fd_, buf.m.offset);
						if (start == MAP_FAILED) {
							VCS_MEDIA_LOG_ERROR("[V4L2VideoSource] mmap(%u) failed: %s",
								i, std::strerror(errno));
							return false;
						}
						buffers_[i].start = start;
						buffers_[i].length = buf.length;

						if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
							VCS_MEDIA_LOG_ERROR("[V4L2VideoSource] VIDIOC_QBUF(%u) failed: %s",
								i, std::strerror(errno));
							return false;
						}
					}

					v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
						VCS_MEDIA_LOG_ERROR("[V4L2VideoSource] VIDIOC_STREAMON failed: %s",
							std::strerror(errno));
						return false;
					}
					streaming_ = true;
					return true;
				}

				void V4L2VideoSource::StopStreaming() {
					if (streaming_ && fd_ >= 0) {
						v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
						ioctl(fd_, VIDIOC_STREAMOFF, &type);
						streaming_ = false;
					}
					for (auto& b : buffers_) {
						if (b.start && b.start != MAP_FAILED) {
							munmap(b.start, b.length);
						}
					}
					buffers_.clear();
				}

				bool V4L2VideoSource::OpenDevice() {
					const std::string path = "/dev/video" + std::to_string(deviceIndex_);
					fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
					if (fd_ < 0) {
						VCS_MEDIA_LOG_ERROR("[V4L2VideoSource] open %s failed: %s",
							path.c_str(), std::strerror(errno));
						return false;
					}

					v4l2_capability cap;
					std::memset(&cap, 0, sizeof(cap));
					if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
						VCS_MEDIA_LOG_ERROR("[V4L2VideoSource] VIDIOC_QUERYCAP failed: %s",
							std::strerror(errno));
						return false;
					}
					const uint32_t caps = cap.device_caps ? cap.device_caps : cap.capabilities;
					if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
						VCS_MEDIA_LOG_ERROR(
							"[V4L2VideoSource] %s is not a streaming capture device", path.c_str());
						return false;
					}

					if (!SetFormat()) return false;

					// 帧率协商是 best-effort：不支持就按驱动默认帧率采集
					v4l2_streamparm parm;
					std::memset(&parm, 0, sizeof(parm));
					parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					parm.parm.capture.timeperframe.numerator = 1;
					parm.parm.capture.timeperframe.denominator = kRequestFps;
					if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
						VCS_MEDIA_LOG_DEBUG(
							"[V4L2VideoSource] VIDIOC_S_PARM(%ufps) not supported, using default",
							kRequestFps);
					}

					if (!StartStreaming()) return false;

					VCS_MEDIA_LOG_INFO("[V4L2VideoSource] opened: id=%s, %ux%u, %s, stride=%u",
						id_.c_str(), width_, height_,
						PixelFormatName(pixelFormat_), stride_);
					return true;
				}

				void V4L2VideoSource::CloseDevice() {
					StopStreaming();
					if (fd_ >= 0) {
						::close(fd_);
						fd_ = -1;
					}
					width_ = height_ = stride_ = 0;
					pixelFormat_ = 0;
				}

				bool V4L2VideoSource::ConvertToI420(const uint8_t* src, size_t size,
					std::shared_ptr<std::vector<uint8_t>>& out) {
					if (!src || size == 0 || width_ == 0 || height_ == 0) return false;

					auto data = std::make_shared<std::vector<uint8_t>>();
					bool ok = false;
					switch (pixelFormat_) {
					case V4L2_PIX_FMT_YUYV:
						ok = YuyvToI420(src, size, width_, height_, stride_, *data);
						break;
					case V4L2_PIX_FMT_NV12:
						ok = Nv12ToI420(src, size, width_, height_, stride_, *data);
						break;
					case V4L2_PIX_FMT_YUV420:
						ok = PlanarToI420(src, size, width_, height_, stride_, false, *data);
						break;
					case V4L2_PIX_FMT_YV12:
						ok = PlanarToI420(src, size, width_, height_, stride_, true, *data);
						break;
					default:
						return false;
					}
					if (!ok) return false;
					out = std::move(data);
					return true;
				}

				void V4L2VideoSource::Broadcast(std::shared_ptr<const VideoFrame> frame) {
					++diagFrames_;
					if (!diagFirstFrameLogged_) {
						diagFirstFrameLogged_ = true;
						VCS_MEDIA_LOG_INFO("[V4L2VideoSource] first frame: id=%s, %ux%u, %zu bytes",
							id_.c_str(), width_, height_, frame->dataSize);
					}

					std::vector<FrameCallback> cbs;
					{
						std::lock_guard<std::mutex> lock(subsMtx_);
						cbs.reserve(subscribers_.size());
						for (auto& kv : subscribers_) cbs.push_back(kv.second);
					}
					for (auto& cb : cbs) {
						try {
							cb(frame);
						}
						catch (const std::exception& e) {
							VCS_MEDIA_LOG_ERROR("[V4L2VideoSource] subscriber threw: %s", e.what());
						}
					}
				}

				void V4L2VideoSource::MaybeLogDiagnostics() {
					const int64_t nowMs = static_cast<int64_t>(vcs::base::platform::UtcNowEpochMs());
					if (diagLastLogMs_ == 0) {
						diagLastLogMs_ = nowMs;
						return;
					}
					if (nowMs - diagLastLogMs_ < 1000) return;
					diagLastLogMs_ = nowMs;

					VCS_MEDIA_LOG_INFO(
						"[V4L2VideoSource] capture diag: id=%s, frames=%llu/s, dequeueErrors=%llu, %ux%u %s",
						id_.c_str(),
						static_cast<unsigned long long>(diagFrames_),
						static_cast<unsigned long long>(diagDequeueErrors_),
						width_, height_, PixelFormatName(pixelFormat_));

					diagFrames_ = 0;
					diagDequeueErrors_ = 0;
				}

				// ============================================================
				// CaptureThreadFunc
				// ============================================================
				void V4L2VideoSource::CaptureThreadFunc() {
					VCS_MEDIA_LOG_INFO("[V4L2VideoSource] CaptureThreadFunc enter: id=%s",
						id_.c_str());

					if (!OpenDevice()) {
						VCS_MEDIA_LOG_ERROR("[V4L2VideoSource] OpenDevice failed: id=%s",
							id_.c_str());
						CloseDevice();
						running_ = false;
						return;
					}

					while (running_.load(std::memory_order_acquire)) {
						if (fd_ < 0) break;

						MaybeLogDiagnostics();

						pollfd pfd;
						pfd.fd = fd_;
						pfd.events = POLLIN;
						pfd.revents = 0;
						const int pr = ::poll(&pfd, 1, kPollTimeoutMs);
						if (pr < 0) {
							if (errno == EINTR) continue;
							VCS_MEDIA_LOG_WARNING("[V4L2VideoSource] poll failed: %s",
								std::strerror(errno));
							std::this_thread::sleep_for(std::chrono::milliseconds(50));
							continue;
						}
						if (pr == 0) continue;

						for (;;) {
							v4l2_buffer buf;
							std::memset(&buf, 0, sizeof(buf));
							buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
							buf.memory = V4L2_MEMORY_MMAP;

							if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
								if (errno == EAGAIN) break;
								++diagDequeueErrors_;
								if (errno == ENODEV) {
									VCS_MEDIA_LOG_ERROR(
										"[V4L2VideoSource] device removed: id=%s", id_.c_str());
									running_ = false;
									break;
								}
								VCS_MEDIA_LOG_WARNING("[V4L2VideoSource] VIDIOC_DQBUF failed: %s",
									std::strerror(errno));
								std::this_thread::sleep_for(std::chrono::milliseconds(50));
								break;
							}

							if (buf.index < buffers_.size() &&
								!(buf.flags & V4L2_BUF_FLAG_ERROR)) {
								const uint8_t* src =
									static_cast<const uint8_t*>(buffers_[buf.index].start);
								const size_t avail = buf.bytesused
									? static_cast<size_t>(buf.bytesused)
									: buffers_[buf.index].length;

								std::shared_ptr<std::vector<uint8_t>> converted;
								if (ConvertToI420(src, avail, converted)) {
									auto frame = std::make_shared<VideoFrame>();
									frame->data = std::move(converted);
									frame->dataSize = frame->data->size();
									frame->width = width_;
									frame->height = height_;
									frame->timestamp = vcs::base::platform::UtcNowEpochMs() * 1000;
									frame->pixelFormat = VideoFrame::PixelFormat::YUV420P;
									frame->memberId = id_;

									Broadcast(frame);
								}
							}

							if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
								VCS_MEDIA_LOG_WARNING("[V4L2VideoSource] VIDIOC_QBUF failed: %s",
									std::strerror(errno));
								break;
							}

							if (!running_.load()) break;
						}
					}

					CloseDevice();

					VCS_MEDIA_LOG_INFO("[V4L2VideoSource] CaptureThreadFunc exit: id=%s",
						id_.c_str());
					running_ = false;
				}

			}
		}
	}
}
