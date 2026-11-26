#pragma once

#include "AudioFrame.h"
#include <functional>
#include <memory>
#include <string>

namespace vcs { namespace media {

/**
 * @brief 音频采集源（一对多）
 */
class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    virtual const std::string& GetId() const = 0;

    using FrameCallback = std::function<void(AudioFramePtr)>;

    virtual void Subscribe(const std::string& consumerId, FrameCallback cb) = 0;
    virtual void Unsubscribe(const std::string& consumerId) = 0;

    virtual size_t GetSubscriberCount() const = 0;
};

}} // namespace vcs::media
