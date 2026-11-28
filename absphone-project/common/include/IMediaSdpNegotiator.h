#pragma once

/**
 * @file IMediaSdpNegotiator.h
 * @brief 应用侧媒体 SDP 协商契约（mediaAppManaged 模式）
 *
 * pjsua 打开 AccountConfig::callConfig.mediaAppManaged 后，协议栈不再生成、
 * 也不校验 SDP，完全由应用提供 offer/answer。本契约就是"应用侧提供 SDP"的
 * 统一入口：
 *   - CreateOffer ：出呼前构造本端 offer
 *   - CreateAnswer：收到对端 offer 后构造本端 answer
 *   - ValidateSdp ：校验一段 SDP 文本（对端 offer、本端生成结果）
 *
 * 契约放在 common：gateway 只需要按接口指针调用，不链接 media；实现在
 * media 模块（IMediaFactory::CreateSdpNegotiator），由组合根注入。
 */

#include "common_export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vcs {
namespace media_config {

/**
 * @brief 生成 SDP 所需的本地媒体参数
 */
struct MediaSdpRequest {
    /// 会话标识（仅用于日志/定位，不写入 SDP）
    std::string callId;
    /// 本端媒体地址（c= 行）
    std::string localIp;
    /// 本端音频 RTP 端口（m=audio，0 表示不可用）
    uint16_t audioPort = 0;
    /// 本端视频 RTP 端口（m=video，0 表示不可用）
    uint16_t videoPort = 0;
    /// 是否提供音频能力
    bool enableAudio = true;
    /// 是否提供视频能力
    bool enableVideo = true;
    /// 对端 offer 带 a=rtcp-mux 时，answer 需镜像该属性
    bool audioRtcpMux = false;
    bool videoRtcpMux = false;
    /// 视频动态载荷类型（默认 97 = H264）
    uint8_t videoPayloadType = 97;
    /// s= 行会话名
    std::string sessionName = "MCU SDP";
};

/**
 * @brief SDP 生成结果
 */
struct MediaSdpResult {
    bool ok = false;
    std::string sdp;
    std::string error;
};

/**
 * @brief SDP 校验结果
 */
struct MediaSdpValidationResult {
    bool valid = false;
    std::vector<std::string> errors;
    unsigned audioMediaCount = 0;
    unsigned videoMediaCount = 0;
};

/**
 * @brief 应用侧 SDP 协商接口
 */
class IMediaSdpNegotiator {
public:
    virtual ~IMediaSdpNegotiator() = default;

    /**
     * @brief 构造本端 offer
     * @param request 本端媒体参数
     * @return ok=true 时 sdp 为可直接放入 INVITE 的 SDP 文本
     */
    virtual MediaSdpResult CreateOffer(const MediaSdpRequest& request) = 0;

    /**
     * @brief 依据对端 offer 构造本端 answer
     * @param request 本端媒体参数
     * @param remoteOffer 对端 SDP offer 文本
     * @return ok=true 时 sdp 为可直接放入 200 OK 的 SDP 文本
     */
    virtual MediaSdpResult CreateAnswer(const MediaSdpRequest& request,
                                        const std::string& remoteOffer) = 0;

    /**
     * @brief 校验 SDP 文本结构
     *
     * 检查会话级必填行（v=/o=/s=/t=）、至少一条 m= 行、m= 行的端口与协议、
     * 动态载荷类型是否有对应 a=rtpmap、媒体 c= 地址是否可用。
     */
    virtual MediaSdpValidationResult ValidateSdp(const std::string& sdp) const = 0;
};

} // namespace media_config
} // namespace vcs
