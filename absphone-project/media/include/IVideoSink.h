#pragma once

#include "VideoFrame.h"
#include <memory>
#include <string>

namespace vcs { namespace media {

/**
 * @brief 视频渲染目标（一对一）
 *
 * 一个 Sink 对应一个物理目标（主窗口 / 辅窗口 / 全屏）。
 * 同一时刻只服务一个流；流 Start 时 BindStream，Stop 时 UnbindStream。
 * 多个流同时 Bind 同一个 Sink 时，第二个 Bind 返回 false。
 */
class IVideoSink {
public:
    virtual ~IVideoSink() = default;

    virtual const std::string& GetId() const = 0;

    virtual bool BindStream(const std::string& streamId) = 0;
    virtual void UnbindStream() = 0;

    virtual bool Render(std::shared_ptr<const VideoFrame> frame) = 0;

    virtual const std::string& GetBoundStreamId() const = 0;
};

}} // namespace vcs::media
