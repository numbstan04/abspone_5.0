#pragma once

#include <string>
#include <cstdint>
#include <cctype>

namespace vcs { namespace gateway {

// 旧文件里的监听端点原文，例如：
//   "SIP Listener=*:50600,transport=*"
//   "H.323 Listener=*:1720"
// host 为空或 "*" 归一为 "0.0.0.0"；端口非法时 hasPort=false，由调用方回退。
struct ListenerSpec {
    std::string host;
    uint16_t port = 0;
    bool hasPort = false;
    std::string transport;
};

namespace listener_detail {

inline std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

inline bool EqualsNoCase(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size(); ++i) {
        if (b[i] == '\0') return false;
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return b[i] == '\0';
}

inline bool ParsePort(const std::string& s, uint16_t* out) {
    if (s.empty()) return false;
    unsigned long v = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isdigit(c)) return false;
        v = v * 10 + static_cast<unsigned long>(c - '0');
        if (v > 65535UL) return false;
    }
    if (v == 0) return false;
    *out = static_cast<uint16_t>(v);
    return true;
}

} // namespace listener_detail

inline bool ParseListenerSpec(const std::string& raw, ListenerSpec* out) {
    if (!out) return false;
    out->host = "0.0.0.0";
    out->port = 0;
    out->hasPort = false;
    out->transport.clear();

    const std::string text = listener_detail::Trim(raw);
    if (text.empty()) return false;

    std::string addrPart = text;
    std::string paramPart;
    const size_t comma = text.find(',');
    if (comma != std::string::npos) {
        addrPart = text.substr(0, comma);
        paramPart = text.substr(comma + 1);
    }

    // 参数区：只认 transport=，未知参数忽略（旧文件还有 transport=* 这类通配）
    size_t pos = 0;
    while (pos < paramPart.size()) {
        const size_t next = paramPart.find(',', pos);
        const std::string item = listener_detail::Trim(
            paramPart.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
        const size_t eq = item.find('=');
        if (eq != std::string::npos &&
            listener_detail::EqualsNoCase(listener_detail::Trim(item.substr(0, eq)), "transport")) {
            out->transport = listener_detail::Trim(item.substr(eq + 1));
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }

    // host[:port]。只有冒号后面全是数字才当端口，避免误判裸 IPv6。
    addrPart = listener_detail::Trim(addrPart);
    std::string hostPart = addrPart;
    std::string portPart;
    const size_t colon = addrPart.rfind(':');
    if (colon != std::string::npos) {
        const std::string tail = listener_detail::Trim(addrPart.substr(colon + 1));
        bool allDigits = !tail.empty();
        for (size_t i = 0; i < tail.size() && allDigits; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(tail[i]))) allDigits = false;
        }
        if (allDigits) {
            hostPart = addrPart.substr(0, colon);
            portPart = tail;
        }
    }

    hostPart = listener_detail::Trim(hostPart);
    out->host = (hostPart.empty() || hostPart == "*") ? "0.0.0.0" : hostPart;

    if (!portPart.empty()) {
        out->hasPort = listener_detail::ParsePort(portPart, &out->port);
    }
    return true;
}

} // namespace gateway
} // namespace vcs
