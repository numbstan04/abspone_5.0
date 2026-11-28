#include "MediaSdpNegotiator.h"
#include "ILogger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace vcs {
namespace media {

namespace {

// SDP 能力集（与 gateway/src/ExternalSdpBuilder.cpp 的输出保持一致）：
//   音频 PCMA(8) / PCMU(0)
//   视频 H264（动态载荷类型，默认 97）
const char* const kAudioPcma = "8";
const char* const kAudioPcmu = "0";
constexpr uint8_t kDefaultVideoPayloadType = 97;

struct ParsedMedia {
    std::string type;                       // audio / video / text ...
    int         port = 0;
    std::string proto;                      // RTP/AVP ...
    std::vector<std::string> formats;       // m= 行载荷类型列表
    std::string connIp;                     // 媒体级 c= 行地址
    bool        rtcpMux = false;
    std::map<std::string, std::string> rtpmap;  // pt -> "H264/90000"
    std::map<std::string, std::string> fmtp;    // pt -> "profile-level-id=...; ..."
};

struct ParsedSdp {
    bool hasVersion = false;
    bool hasOrigin = false;
    bool hasName = false;
    bool hasTime = false;
    std::string connIp;                     // 会话级 c= 行地址
    std::vector<ParsedMedia> media;
};

std::string Trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        if (current.back() == '\r') {
            current.pop_back();
        }
        lines.push_back(current);
    }
    return lines;
}

bool IsNumericPayloadType(const std::string& fmt, int* out) {
    if (fmt.empty()) {
        return false;
    }
    int value = 0;
    for (char c : fmt) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        value = value * 10 + (c - '0');
        if (value > 127) {
            return false;
        }
    }
    if (out) {
        *out = value;
    }
    return true;
}

ParsedSdp ParseSdp(const std::string& text) {
    ParsedSdp parsed;
    ParsedMedia* current = nullptr;

    for (const std::string& rawLine : SplitLines(text)) {
        const std::string line = Trim(rawLine);
        if (line.size() < 2 || line[1] != '=') {
            continue;
        }
        const char key = line[0];
        const std::string value = Trim(line.substr(2));

        switch (key) {
            case 'v':
                parsed.hasVersion = true;
                break;
            case 'o':
                parsed.hasOrigin = true;
                break;
            case 's':
                parsed.hasName = true;
                break;
            case 't':
                parsed.hasTime = true;
                break;
            case 'c': {
                std::istringstream iss(value);
                std::string netType, addrType, addr;
                iss >> netType >> addrType >> addr;
                if (netType == "IN" && !addr.empty()) {
                    if (current) {
                        if (current->connIp.empty()) {
                            current->connIp = addr;
                        }
                    } else {
                        parsed.connIp = addr;
                    }
                }
                break;
            }
            case 'm': {
                ParsedMedia media;
                std::istringstream iss(value);
                std::string portToken;
                iss >> media.type >> portToken >> media.proto;
                // 端口可能带 "/2" 计数后缀
                const size_t slash = portToken.find('/');
                if (slash != std::string::npos) {
                    portToken = portToken.substr(0, slash);
                }
                media.port = std::atoi(portToken.c_str());
                std::string fmt;
                while (iss >> fmt) {
                    media.formats.push_back(fmt);
                }
                parsed.media.push_back(media);
                current = &parsed.media.back();
                break;
            }
            case 'a': {
                if (!current) {
                    break;
                }
                const size_t colon = value.find(':');
                const std::string name = colon == std::string::npos
                    ? value : value.substr(0, colon);
                const std::string attrValue = colon == std::string::npos
                    ? std::string() : Trim(value.substr(colon + 1));

                if (name == "rtcp-mux") {
                    current->rtcpMux = true;
                } else if (name == "rtpmap" || name == "fmtp") {
                    const size_t space = attrValue.find(' ');
                    if (space == std::string::npos) {
                        break;
                    }
                    const std::string pt = Trim(attrValue.substr(0, space));
                    const std::string rest = Trim(attrValue.substr(space + 1));
                    if (name == "rtpmap") {
                        current->rtpmap[pt] = rest;
                    } else {
                        current->fmtp[pt] = rest;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    return parsed;
}

bool IsH264Rtpmap(const std::string& value) {
    const std::string upper = [&value]() {
        std::string s = value;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return s;
    }();
    return upper.find("H264/") != std::string::npos;
}

std::string FmtpParam(const std::string& fmtp, const std::string& key) {
    std::istringstream iss(fmtp);
    std::string item;
    while (std::getline(iss, item, ';')) {
        item = Trim(item);
        if (item.compare(0, key.size(), key) == 0) {
            return Trim(item.substr(key.size()));
        }
    }
    return std::string();
}

std::string BuildSessionHeader(const vcs::media_config::MediaSdpRequest& request) {
    std::ostringstream oss;
    const long long now = static_cast<long long>(std::time(nullptr));
    oss << "v=0\r\n";
    oss << "o=mcu " << now << " " << now << " IN IP4 " << request.localIp << "\r\n";
    oss << "s=" << (request.sessionName.empty() ? "MCU SDP" : request.sessionName) << "\r\n";
    oss << "c=IN IP4 " << request.localIp << "\r\n";
    oss << "t=0 0\r\n";
    return oss.str();
}

std::string BuildH264Fmtp(const ParsedMedia& media, const std::string& pt) {
    std::string profileLevelId = "42e01f";
    std::string maxMbps;
    std::string maxFs;

    const auto it = media.fmtp.find(pt);
    if (it != media.fmtp.end()) {
        const std::string remoteProfile = FmtpParam(it->second, "profile-level-id=");
        if (!remoteProfile.empty()) {
            profileLevelId = remoteProfile;
        }
        maxMbps = FmtpParam(it->second, "max-mbps=");
        maxFs = FmtpParam(it->second, "max-fs=");
    }

    std::ostringstream oss;
    oss << "a=fmtp:" << pt << " profile-level-id=" << profileLevelId;
    if (!maxMbps.empty()) {
        oss << "; max-mbps=" << maxMbps;
    }
    if (!maxFs.empty()) {
        oss << "; max-fs=" << maxFs;
    }
    oss << "; packetization-mode=1\r\n";
    return oss.str();
}

} // namespace

vcs::media_config::MediaSdpValidationResult MediaSdpNegotiator::ValidateSdp(
    const std::string& sdp) const {
    vcs::media_config::MediaSdpValidationResult result;

    if (sdp.empty()) {
        result.errors.push_back("SDP is empty");
        return result;
    }

    const ParsedSdp parsed = ParseSdp(sdp);

    if (!parsed.hasVersion) result.errors.push_back("missing v= line");
    if (!parsed.hasOrigin)  result.errors.push_back("missing o= line");
    if (!parsed.hasName)    result.errors.push_back("missing s= line");
    if (!parsed.hasTime)    result.errors.push_back("missing t= line");
    if (parsed.media.empty()) result.errors.push_back("no m= line");

    for (size_t i = 0; i < parsed.media.size(); ++i) {
        const ParsedMedia& media = parsed.media[i];
        const std::string prefix = "m= line #" + std::to_string(i);

        if (media.type.empty()) {
            result.errors.push_back(prefix + ": missing media type");
        }
        if (media.port < 0 || media.port > 65535) {
            result.errors.push_back(prefix + ": invalid port");
        }
        // 传输协议只要求非空：offer 里常见 m=application UDP/BFCP（BFCP 内容共享），
        // 这类 m-line 不是 RTP，仍必须出现在 answer 里（端口 0 拒绝）。
        if (media.proto.empty()) {
            result.errors.push_back(prefix + ": missing transport");
        }
        if (media.formats.empty()) {
            result.errors.push_back(prefix + ": empty payload type list");
        }
        if (media.port > 0 && media.connIp.empty() && parsed.connIp.empty()) {
            result.errors.push_back(prefix + ": no connection address (c=)");
        }

        for (const std::string& fmt : media.formats) {
            int payloadType = 0;
            if (!IsNumericPayloadType(fmt, &payloadType)) {
                // 非数字 fmt（如 BFCP 的 "*"）按 pjmedia 规则忽略。
                continue;
            }
            // RTP 媒体的动态载荷类型（96..127）必须用 rtpmap 声明编码
            if (payloadType >= 96 && media.proto.rfind("RTP/", 0) == 0 &&
                media.rtpmap.find(fmt) == media.rtpmap.end()) {
                result.errors.push_back(prefix + ": dynamic payload " + fmt + " has no a=rtpmap");
            }
        }

        if (media.type == "audio") {
            ++result.audioMediaCount;
        } else if (media.type == "video") {
            ++result.videoMediaCount;
        }
    }

    result.valid = result.errors.empty();
    return result;
}

vcs::media_config::MediaSdpResult MediaSdpNegotiator::CreateOffer(
    const vcs::media_config::MediaSdpRequest& request) {
    vcs::media_config::MediaSdpResult result;

    if (request.localIp.empty()) {
        result.error = "localIp is empty";
        return result;
    }
    if (!request.enableAudio && !request.enableVideo) {
        result.error = "both audio and video are disabled";
        return result;
    }
    if (request.enableAudio && request.audioPort == 0) {
        result.error = "audio is enabled but audioPort is 0";
        return result;
    }
    if (request.enableVideo && request.videoPort == 0) {
        result.error = "video is enabled but videoPort is 0";
        return result;
    }

    std::ostringstream oss;
    oss << BuildSessionHeader(request);

    if (request.enableAudio) {
        oss << "m=audio " << request.audioPort << " RTP/AVP "
            << kAudioPcma << " " << kAudioPcmu << "\r\n";
        oss << "a=rtpmap:" << kAudioPcma << " PCMA/8000\r\n";
        oss << "a=rtpmap:" << kAudioPcmu << " PCMU/8000\r\n";
        oss << "a=sendrecv\r\n";
        if (request.audioRtcpMux) {
            oss << "a=rtcp-mux\r\n";
        }
    }

    if (request.enableVideo) {
        const std::string pt = std::to_string(static_cast<int>(request.videoPayloadType));
        oss << "m=video " << request.videoPort << " RTP/AVP " << pt << "\r\n";
        oss << "a=rtpmap:" << pt << " H264/90000\r\n";
        oss << "a=fmtp:" << pt << " profile-level-id=42e01f; packetization-mode=1\r\n";
        // RFC 4585/5104：声明 FIR / PLI 关键帧请求能力，否则对端可合法忽略。
        oss << "a=rtcp-fb:" << pt << " ccm fir\r\n";
        oss << "a=rtcp-fb:" << pt << " nack pli\r\n";
        oss << "a=sendrecv\r\n";
        if (request.videoRtcpMux) {
            oss << "a=rtcp-mux\r\n";
        }
    }

    result.sdp = oss.str();
    result.ok = true;

    VCS_MEDIA_LOG_INFO("[MediaSdp] offer created: callId=%s, audio=%u, video=%u",
                       request.callId.c_str(),
                       static_cast<unsigned>(request.enableAudio ? request.audioPort : 0),
                       static_cast<unsigned>(request.enableVideo ? request.videoPort : 0));
    return result;
}

vcs::media_config::MediaSdpResult MediaSdpNegotiator::CreateAnswer(
    const vcs::media_config::MediaSdpRequest& request,
    const std::string& remoteOffer) {
    vcs::media_config::MediaSdpResult result;

    const vcs::media_config::MediaSdpValidationResult validation = ValidateSdp(remoteOffer);
    if (!validation.valid) {
        result.error = "remote offer is invalid";
        for (const std::string& error : validation.errors) {
            result.error += "; " + error;
        }
        return result;
    }
    if (request.localIp.empty()) {
        result.error = "localIp is empty";
        return result;
    }

    const ParsedSdp parsed = ParseSdp(remoteOffer);
    std::ostringstream oss;
    oss << BuildSessionHeader(request);

    // answer 的 m-line 数量必须和 offer 完全一致：pjsua 的
    // validate_app_sdp() 在 REMOTE_OFFER 状态下会比对 media_count，
    // 少一条直接回 PJMEDIA_SDPNEG_EMISMEDIA。所以这里逐条镜像 offer，
    // 只接通第一条音频和第一条视频，其余 m-line 用端口 0 明确拒绝。
    bool audioAnswered = false;
    bool videoAnswered = false;

    for (const ParsedMedia& media : parsed.media) {
        // ---- 第一条音频 ----
        if (media.type == "audio" && !audioAnswered) {
            audioAnswered = true;

            std::vector<std::string> accepted;
            if (request.enableAudio && request.audioPort > 0) {
                // 本端偏好：PCMA 优先，其次 PCMU；只保留对端也 offer 的载荷类型。
                for (const char* candidate : {kAudioPcma, kAudioPcmu}) {
                    if (std::find(media.formats.begin(), media.formats.end(), candidate)
                            != media.formats.end()) {
                        accepted.emplace_back(candidate);
                    }
                }
            }

            if (accepted.empty()) {
                const std::string fmt = media.formats.empty() ? "0" : media.formats.front();
                oss << "m=audio 0 RTP/AVP " << fmt << "\r\n";
                oss << "a=inactive\r\n";
                VCS_MEDIA_LOG_INFO("[MediaSdp] answer: audio rejected (no common codec), callId=%s",
                                   request.callId.c_str());
            } else {
                oss << "m=audio " << request.audioPort << " RTP/AVP";
                for (const std::string& fmt : accepted) {
                    oss << " " << fmt;
                }
                oss << "\r\n";
                for (const std::string& fmt : accepted) {
                    if (fmt == kAudioPcma) {
                        oss << "a=rtpmap:" << kAudioPcma << " PCMA/8000\r\n";
                    } else {
                        oss << "a=rtpmap:" << kAudioPcmu << " PCMU/8000\r\n";
                    }
                }
                oss << "a=sendrecv\r\n";
                if (media.rtcpMux) {
                    oss << "a=rtcp-mux\r\n";
                }
            }
            continue;
        }

        // ---- 第一条视频 ----
        if (media.type == "video" && !videoAnswered) {
            videoAnswered = true;

            std::string h264Pt;
            if (request.enableVideo && request.videoPort > 0) {
                // answer 必须复用 offer 的载荷类型编号。
                for (const std::string& fmt : media.formats) {
                    const auto it = media.rtpmap.find(fmt);
                    if (it != media.rtpmap.end() && IsH264Rtpmap(it->second)) {
                        h264Pt = fmt;
                        break;
                    }
                }
            }

            if (h264Pt.empty()) {
                const std::string fmt = media.formats.empty() ? "97" : media.formats.front();
                oss << "m=video 0 RTP/AVP " << fmt << "\r\n";
                oss << "a=inactive\r\n";
                VCS_MEDIA_LOG_INFO("[MediaSdp] answer: video rejected (no H264 offer), callId=%s",
                                   request.callId.c_str());
            } else {
                oss << "m=video " << request.videoPort << " RTP/AVP " << h264Pt << "\r\n";
                oss << "a=rtpmap:" << h264Pt << " H264/90000\r\n";
                oss << BuildH264Fmtp(media, h264Pt);
                oss << "a=rtcp-fb:" << h264Pt << " ccm fir\r\n";
                oss << "a=rtcp-fb:" << h264Pt << " nack pli\r\n";
                oss << "a=sendrecv\r\n";
                if (media.rtcpMux) {
                    oss << "a=rtcp-mux\r\n";
                }
            }
            continue;
        }

        // ---- 其余 m-line（第二路视频、文本等）----
        // 保留占位以匹配 offer 的 media_count，端口 0 = 拒绝。
        const std::string fmt = media.formats.empty() ? "0" : media.formats.front();
        oss << "m=" << media.type << " 0 "
            << (media.proto.empty() ? "RTP/AVP" : media.proto) << " " << fmt << "\r\n";
        oss << "a=inactive\r\n";
    }

    if (!audioAnswered && !videoAnswered) {
        result.error = "remote offer has neither audio nor video media line";
        return result;
    }

    result.sdp = oss.str();
    result.ok = true;

    VCS_MEDIA_LOG_INFO("[MediaSdp] answer created: callId=%s, audio=%s, video=%s, m-lines=%zu",
                       request.callId.c_str(),
                       audioAnswered ? "yes" : "no",
                       videoAnswered ? "yes" : "no",
                       parsed.media.size());
    return result;
}

} // namespace media
} // namespace vcs
