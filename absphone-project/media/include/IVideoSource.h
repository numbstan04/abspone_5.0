#pragma once

#include "VideoFrame.h"
#include <functional>
#include <memory>
#include <string>

namespace vcs { namespace media {

/**
 * @brief 视频采集源（一对多）
 *
 * 一个 Source 对应一个物理设备（Camera / Screen Capture / File）。
 * 多个流可以订阅同一个 Source；采集线程每产生一帧，广播给所有订阅者。
 *
 * 线程模型：
 *   - 第一个 Subscribe 启动采集线程
 *   - 最后一个 Unsubscribe 停止采集线程
 *   - 回调在采集线程执行，订阅者必须快速返回（耗时操作入队到自己的线程）
 */
class IVideoSource {
public:
    virtual ~IVideoSource() = default;

    virtual const std::string& GetId() const = 0;

    using FrameCallback = std::function<void(std::shared_ptr<const VideoFrame>)>;

    virtual void Subscribe(const std::string& consumerId, FrameCallback cb) = 0;
    virtual void Unsubscribe(const std::string& consumerId) = 0;

    virtual size_t GetSubscriberCount() const = 0;
};

}} // namespace vcs::media
