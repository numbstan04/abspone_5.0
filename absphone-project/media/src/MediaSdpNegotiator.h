#pragma once

/**
 * @file MediaSdpNegotiator.h
 * @brief 应用侧 SDP 协商实现（纯文本，不依赖 pjproject）
 *
 * media 模块不链接 pjsip，因此这里只做 SDP 文本的构造与校验。
 * gateway 拿到文本后直接交给 pjsua（CallOpParam::sdp.wholeSdp）或
 * pjmedia_sdp_parse 解析成协议栈对象。
 */

#include "IMediaSdpNegotiator.h"

namespace vcs {
namespace media {

class MediaSdpNegotiator final : public vcs::media_config::IMediaSdpNegotiator {
public:
    MediaSdpNegotiator() = default;
    ~MediaSdpNegotiator() override = default;

    vcs::media_config::MediaSdpResult CreateOffer(
        const vcs::media_config::MediaSdpRequest& request) override;

    vcs::media_config::MediaSdpResult CreateAnswer(
        const vcs::media_config::MediaSdpRequest& request,
        const std::string& remoteOffer) override;

    vcs::media_config::MediaSdpValidationResult ValidateSdp(
        const std::string& sdp) const override;
};

} // namespace media
} // namespace vcs
