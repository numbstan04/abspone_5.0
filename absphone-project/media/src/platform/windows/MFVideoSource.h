#pragma once

#include "IVideoSource.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// 前向声明（避免在头文件里拉 Windows/MF 头）
struct IMFMediaSource;
struct IMFSourceReader;

namespace vcs {
	namespace media {
		namespace platform {
			namespace windows {

				/**
				 * @brief MediaFoundation 摄像头采集
				 *
				 * 用 IMFSourceReader 读帧，输出 YUV420P。
				 *
				 * 线程模型：
				 *   - 第一个 Subscribe 启动采集线程
				 *   - 最后一个 Unsubscribe 停止采集线程
				 *   - 回调在采集线程执行，订阅者必须快速返回
				 *
				 * COM 生命周期：
				 *   - 采集线程内部调用 CoInitializeEx(MTA) / CoUninitialize
				 *   - 所有 MF 对象（IMFMediaSource / IMFSourceReader）只在采集线程
				 *     上创建和销毁，避免跨套间问题
				 */
				class MFVideoSource : public IVideoSource {
				public:
					explicit MFVideoSource(int deviceIndex);
					~MFVideoSource() override;

					const std::string& GetId() const override { return id_; }

					void Subscribe(const std::string& consumerId, FrameCallback cb) override;
					void Unsubscribe(const std::string& consumerId) override;

					size_t GetSubscriberCount() const override;

				private:
					void CaptureThreadFunc();
					bool OpenDevice();
					void CloseDevice();

					int deviceIndex_;
					std::string id_;

					// MF 对象（只在采集线程访问）
					IMFMediaSource*  mediaSource_ = nullptr;
					IMFSourceReader* reader_ = nullptr;
					uint32_t width_ = 0;
					uint32_t height_ = 0;

					// 订阅者
					mutable std::mutex subsMtx_;
					std::map<std::string, FrameCallback> subscribers_;

					// 采集线程
					std::thread captureThread_;
					std::atomic<bool> running_{ false };

					// 启动/停止互斥（防止并发 Subscribe 触发重复启动）
					std::mutex startStopMtx_;
				};

			}
		}
	}
}