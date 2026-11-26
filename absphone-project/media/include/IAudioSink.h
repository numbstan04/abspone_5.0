#pragma once

#include "AudioFrame.h"
#include <memory>
#include <string>

namespace vcs { namespace media {

/**
 * @brief 音频渲染目标（一对一）
 */
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    virtual const std::string& GetId() const = 0;

    virtual bool BindStream(const std::string& streamId) = 0;
    virtual void UnbindStream() = 0;

    virtual bool Render(AudioFramePtr frame) = 0;

    virtual const std::string& GetBoundStreamId() const = 0;
};

}} // namespace vcs::media
