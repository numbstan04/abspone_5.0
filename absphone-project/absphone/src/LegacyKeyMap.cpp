#include "LegacyKeyMap.h"

namespace vcs { namespace absphone {

namespace {

typedef vcs::base::Config::ConfigKeyAlias Alias;

Alias MakeAlias(const char* canonical, const char* section, const char* legacyKey,
                vcs::base::Config::AliasTransform transform) {
    Alias a;
    a.canonical = canonical;
    a.section = section;
    a.legacyKey = legacyKey;
    a.transform = transform;
    return a;
}

} // namespace

std::vector<vcs::base::Config::ConfigKeyAlias> BuildLegacyKeyAliases() {
    const vcs::base::Config::AliasTransform kNone =
        vcs::base::Config::AliasTransform::None;
    const vcs::base::Config::AliasTransform kInvert =
        vcs::base::Config::AliasTransform::InvertBool;
    const vcs::base::Config::AliasTransform kSrtpUse =
        vcs::base::Config::AliasTransform::SrtpUse;
    const vcs::base::Config::AliasTransform kSrtpAlg =
        vcs::base::Config::AliasTransform::SrtpAlg;
    const vcs::base::Config::AliasTransform kGkMode =
        vcs::base::Config::AliasTransform::GatekeeperMode;

    std::vector<Alias> aliases;
    aliases.reserve(64);

    // A-1 .. A-5 日志与媒体加密
    aliases.push_back(MakeAlias("log.enabled", "Parameters", "EnableLog", kNone));
    aliases.push_back(MakeAlias("log.level", "Parameters", "LogLevel", kNone));
    aliases.push_back(MakeAlias("log_level", "Parameters", "LogLevel", kNone));
    aliases.push_back(MakeAlias("log.size_mb", "Parameters", "LogSize", kNone));
    aliases.push_back(MakeAlias("log_size", "Parameters", "LogSize", kNone));
    aliases.push_back(MakeAlias("media_encryption_enabled", "Parameters",
                                "EnableMediaEncryption", kNone));
    aliases.push_back(MakeAlias("h235_alg_type", "Parameters",
                                "EnableMediaEncryptionAlgorithm", kNone));

    // A-6 .. A-11 传输与媒体能力
    aliases.push_back(MakeAlias("fec_enabled", "Parameters", "EnableFEC", kNone));
    aliases.push_back(MakeAlias("rtp_port_detection_enabled", "Parameters",
                                "SendPortDetection", kNone));
    aliases.push_back(MakeAlias("h245_tunneling_enabled", "Parameters",
                                "Disable H.245 Tunneling", kInvert));
    aliases.push_back(MakeAlias("fast_start_enabled", "Parameters",
                                "Disable Fast-Start", kInvert));
    aliases.push_back(MakeAlias("max_rtp_payload_size", "Parameters",
                                "MaxRTPPayloadSize", kNone));
    aliases.push_back(MakeAlias("auto_matching_video_capability", "Parameters",
                                "AutoMatchingVideoCapability", kNone));

    // A-12 .. A-17 Gatekeeper
    aliases.push_back(MakeAlias("gk_enabled", "Parameters", "Gatekeeper Mode", kGkMode));
    aliases.push_back(MakeAlias("gk_ip", "Parameters", "Gatekeeper", kNone));
    // 首 token 语义由 GetGKParameters 复刻（absphone_impl.cxx:5153-5154），表本身直通。
    aliases.push_back(MakeAlias("gk_reg_number", "Parameters",
                                "Gatekeeper Room Names", kNone));
    aliases.push_back(MakeAlias("gk_reg_name", "Parameters", "Gatekeeper Username", kNone));
    aliases.push_back(MakeAlias("gk_reg_password", "Parameters", "Gatekeeper Password", kNone));
    aliases.push_back(MakeAlias("is_local_gk", "Parameters", "Use Local GK", kNone));

    // A-18 .. A-25 NAT 与端口
    aliases.push_back(MakeAlias("nat_enabled", "Parameters", "EnableNATRouter", kNone));
    aliases.push_back(MakeAlias("nat_ip", "Parameters", "NAT Router IP", kNone));
    aliases.push_back(MakeAlias("ports_setting_enabled", "Parameters",
                                "EnablePortsSetting", kNone));
    aliases.push_back(MakeAlias("tcp_port_base", "Parameters", "TCP Base Port", kNone));
    aliases.push_back(MakeAlias("tcp_port_max", "Parameters", "TCP Max Port", kNone));
    aliases.push_back(MakeAlias("rtp_port_base", "Parameters", "RTP Base Port", kNone));
    aliases.push_back(MakeAlias("rtp_port_max", "Parameters", "RTP Max Port", kNone));
    aliases.push_back(MakeAlias("sip_listen_port", "Parameters", "SipPort", kNone));

    // A-26 .. A-28 终端身份与能力
    aliases.push_back(MakeAlias("username", "Parameters", "Username", kNone));
    aliases.push_back(MakeAlias("default_room_name", "Parameters", "Default room", kNone));
    aliases.push_back(MakeAlias("h264_extend_capability", "Parameters",
                                "EnableH264ExtendCapabilityInfo", kNone));

    // A-29 .. A-37 系统、网络、NAT 穿透
    aliases.push_back(MakeAlias("system.language", "Parameters", "SystemLanguage", kNone));
    aliases.push_back(MakeAlias("network.dhcp", "Parameters", "EnableDHCP", kNone));
    aliases.push_back(MakeAlias("nat.ice", "Parameters", "EnableICE", kNone));
    aliases.push_back(MakeAlias("nat.stun", "Parameters", "EnableMediaSTUN", kNone));
    aliases.push_back(MakeAlias("nat.stun.server", "Parameters", "StunServers", kNone));
    aliases.push_back(MakeAlias("nat.turn", "Parameters", "EnableTURN", kNone));
    aliases.push_back(MakeAlias("nat.turn.server", "Parameters", "TurnServer", kNone));
    aliases.push_back(MakeAlias("nat.turn.username", "Parameters", "TurnUsername", kNone));
    aliases.push_back(MakeAlias("nat.turn.password", "Parameters", "TurnPassword", kNone));

    // A-38 .. A-45 音频
    aliases.push_back(MakeAlias("audio.input.device", "Parameters",
                                "AudioRecordDevice", kNone));
    aliases.push_back(MakeAlias("audio.output.device", "Parameters",
                                "AudioPlayoutDevice", kNone));
    aliases.push_back(MakeAlias("audio.mic.volume", "Parameters",
                                "MicrophoneVolume", kNone));
    aliases.push_back(MakeAlias("audio.speaker.volume", "Parameters",
                                "SpeakerVolume", kNone));
    aliases.push_back(MakeAlias("audio.codec", "Parameters", "AudioCodec", kNone));
    aliases.push_back(MakeAlias("audio.aec", "Parameters", "AudioEnableAec", kNone));
    aliases.push_back(MakeAlias("audio.ns", "Parameters", "AudioEnableNs", kNone));
    aliases.push_back(MakeAlias("audio.agc", "Parameters", "AudioEnableAgc", kNone));

    // A-46 .. A-52 SIP / SRTP
    aliases.push_back(MakeAlias("sip.server", "Parameters", "Sip Proxy Domain", kNone));
    aliases.push_back(MakeAlias("sip.transport", "SIP Parameters", "SipTransport", kNone));
    aliases.push_back(MakeAlias("sip.port", "Parameters", "SipPort", kNone));
    aliases.push_back(MakeAlias("h323.gk", "Parameters", "Gatekeeper", kNone));
    aliases.push_back(MakeAlias("sip.password", "Parameters", "Sip Password", kNone));
    aliases.push_back(MakeAlias("srtp.enable", "Parameters", "MeidaSRTP", kSrtpUse));
    aliases.push_back(MakeAlias("srtp.algorithm", "Parameters", "SRTPAlgorithm", kSrtpAlg));

    // A-53 .. A-56 QoS 与监听端点原文
    aliases.push_back(MakeAlias("qos.enable", "Parameters", "QoSEnabled", kNone));
    aliases.push_back(MakeAlias("qos.priority", "Parameters", "QoSPrecedenceLevel", kNone));
    aliases.push_back(MakeAlias("sip.listener.raw", "SIP Parameters", "SIP Listener", kNone));
    aliases.push_back(MakeAlias("h323.listener.raw", "Parameters", "H.323 Listener", kNone));

    // A-57 .. A-60 夹具缺省的旧键（旧文件没有时只在内存中）
    aliases.push_back(MakeAlias("rs485_enabled", "Parameters", "RS485Control", kNone));
    aliases.push_back(MakeAlias("video_jitter_buffer_enabled", "Parameters",
                                "EnableVideoJitter", kNone));
    aliases.push_back(MakeAlias("audio_jitter_buffer_enabled", "Parameters",
                                "EnableAudioJitter", kNone));
    aliases.push_back(MakeAlias("send_grq_enabled", "Parameters", "EnableSendGRQ", kNone));

    return aliases;
}

} // namespace absphone
} // namespace vcs
