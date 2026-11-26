#pragma once

#include "IPlatformMediaFactory.h"
#include "IVideoSource.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcs {
	namespace media {
		namespace platform {
			namespace linux {

				/**
				 * @brief V4L2 摄像头采集
				 *
				 * 输出 YUV420P（I420），尺寸以驱动实际接受值为准；编码器会跟随
				 * 采集帧尺寸（VideoProcessor::ResizeEncoderToFrame），因此不需要
				 * 在这里强制缩放。
				 *
				 * 支持的输入像素格式：YUYV / NV12 / I420 / YV12。
				 * MJPEG 未纳入：它需要每帧解码，会把编解码依赖拉进平台层；
				 * 打开失败时日志会列出驱动支持的格式，便于换设备或换模式。
				 *
				 * 设备 id 就是设备节点号：camera:0 → /dev/video0。
				 * 线程模型与 Windows 侧一致：第一个 Subscribe 启动，最后一个
				 * Unsubscribe join；设备在采集线程内打开、关闭。
				 */
				class V4L2VideoSource : public IVideoSource {
				public:
					explicit V4L2VideoSource(int deviceIndex);
					~V4L2VideoSource() override;

					const std::string& GetId() const override { return id_; }

					void Subscribe(const std::string& consumerId, FrameCallback cb) override;
					void Unsubscribe(const std::string& consumerId) override;

					size_t GetSubscriberCount() const override;

				private:
					struct Buffer {
						void* start = nullptr;
						size_t length = 0;
					};

					void CaptureThreadFunc();
					bool OpenDevice();
					void CloseDevice();
					bool SetFormat();
					bool StartStreaming();
					void StopStreaming();
					void LogSupportedFormats();
					bool ConvertToI420(const uint8_t* src, size_t size,
						std::shared_ptr<std::vector<uint8_t>>& out);
					void Broadcast(std::shared_ptr<const VideoFrame> frame);
					void MaybeLogDiagnostics();

					int deviceIndex_ = 0;
					int fd_ = -1;
					std::string id_;

					uint32_t width_ = 0;
					uint32_t height_ = 0;
					uint32_t stride_ = 0;
					uint32_t pixelFormat_ = 0;
					bool streaming_ = false;

					std::vector<Buffer> buffers_;

					// 1 Hz 采集诊断
					uint64_t diagFrames_ = 0;
					uint64_t diagDequeueErrors_ = 0;
					int64_t  diagLastLogMs_ = 0;
					bool     diagFirstFrameLogged_ = false;

					mutable std::mutex subsMtx_;
					std::map<std::string, FrameCallback> subscribers_;

					std::thread captureThread_;
					std::atomic<bool> running_{ false };
					std::mutex startStopMtx_;
				};

				// 扫描 /dev/videoN，返回真正支持"采集 + 流式"的节点。
				// id 使用节点号（camera:N → /dev/videoN），显示名为驱动报告的名字。
				std::vector<VideoSourceInfo> EnumerateVideoCaptureDevices();

			}
		}
	}
}
