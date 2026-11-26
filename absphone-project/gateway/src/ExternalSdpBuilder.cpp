/**
 * @file ExternalSdpBuilder.cpp
 * @brief External SDP Builder Implementation
 */

#include "ExternalSdpBuilder.h"
#include "SipSignalingHandler.h"  // Contains RemoteMediaInfo definition
#include "ILogger.h"
#include <pjsua-lib/pjsua_internal.h>
#include <pjlib.h>
#include <cstring>

namespace vcs { namespace gateway {

pj::SdpSession ExternalSdpBuilder::fromPjMediaSdp(pjmedia_sdp_session* sdp) {
    pj::SdpSession result;
    result.fromPj(*sdp);
    return result;
}

// [Key Fix] Add 3-parameter overload, called by SipCall::Answer
pj::SdpSession ExternalSdpBuilder::buildOfferSdp(
    const std::string& localIp,
    uint16_t audioPort,
    uint16_t videoPort) {
    // Internally call the 4-parameter version with remoteInfo = nullptr
    return buildOfferSdp(localIp, audioPort, videoPort, nullptr);
}

pj::SdpSession ExternalSdpBuilder::buildOfferSdp(
    const std::string& localIp,
    uint16_t audioPort,
    uint16_t videoPort,
    const RemoteMediaInfo* remoteInfo) {

    // [Key Fix] Use PJSUA's memory pool rather than a temporary one.
    // A temporary pool would invalidate the SDP by the time pjsua_call_answer_with_sdp clones it.
    // PJSUA's pool is valid for the entire call lifetime.

    // Use pjsua_var's pool (PJSUA's main memory pool)
    pj_pool_t* pool = pjsua_var.pool;

    // Create the SDP session
    pjmedia_sdp_session* sdp = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_session);

    // Session origin (o= line)
    pj_time_val now;
    pj_gettimeofday(&now);
    sdp->origin.user = pj_str((char*)"mcu");
    sdp->origin.id = (pj_uint_t)now.sec;
    sdp->origin.version = (pj_uint_t)now.sec;
    sdp->origin.net_type = pj_str((char*)"IN");
    sdp->origin.addr_type = pj_str((char*)"IP4");
    sdp->origin.addr = pj_str((char*)localIp.c_str());

    // Session name (s= line)
    sdp->name = pj_str((char*)"MCU SDP");

    // Session time (t= line) - default values
    sdp->time.start = 0;
    sdp->time.stop = 0;

    // Connection information (session-level c= line)
    sdp->conn = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_conn);
    addConnectionInfo(sdp->conn, pool, localIp);

    // Add audio media
    addAudioMedia(sdp, pool, localIp, audioPort,
                  remoteInfo ? remoteInfo->audioRtcpMux : false);

    // Add video media (pass remoteInfo to use H.264 parameters)
    addVideoMedia(sdp, pool, localIp, videoPort, remoteInfo);

    // Convert to pj::SdpSession
    pj::SdpSession result;
    result.fromPj(*sdp);

    // [Key] Do NOT release the memory pool! Keep it valid after Answer() returns.
    // The lifetime of pjsua_var.pool is managed by PJSIP.

    VCS_GATEWAY_LOG_INFO("[ExternalSdp] Built offer SDP: audio=%d, video=%d", audioPort, videoPort);

    return result;
}

pj::SdpSession ExternalSdpBuilder::buildAnswerSdp(
    const RemoteMediaInfo& remoteInfo,
    const std::string& localIp,
    uint16_t audioPort,
    uint16_t videoPort) {

    // Answer SDP: pass remoteInfo to use H.264 parameters
    return buildOfferSdp(localIp, audioPort, videoPort, &remoteInfo);
}

void ExternalSdpBuilder::addAudioMedia(pjmedia_sdp_session* sdp,
                                       pj_pool_t* pool,
                                       const std::string& localIp,
                                       uint16_t port,
                                       bool rtcpMux) {

    if (sdp->media_count >= PJMEDIA_MAX_SDP_MEDIA) {
        VCS_GATEWAY_LOG_WARNING("[ExternalSdp] Max media lines reached");
        return;
    }

    pjmedia_sdp_media* media = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_media);

    // m=audio <port> RTP/AVP <payload_types>
    media->desc.media = pj_str((char*)"audio");
    media->desc.port = port;
    media->desc.port_count = 1;
    media->desc.transport = pj_str((char*)"RTP/AVP");
    media->desc.fmt_count = 2;
    media->desc.fmt[0] = pj_str((char*)"8");   // PCMA
    media->desc.fmt[1] = pj_str((char*)"0");   // PCMU

    // Connection information (media-level c= line)
    media->conn = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_conn);
    addConnectionInfo(media->conn, pool, localIp);

    // rtpmap: PCMA/8000
    char rtpmap_buf[64];
    snprintf(rtpmap_buf, sizeof(rtpmap_buf), "8 PCMA/8000");
    pj_str_t rtpmap_val = pj_str(rtpmap_buf);
    pjmedia_sdp_attr* rtpmap = pjmedia_sdp_attr_create(pool, "rtpmap", &rtpmap_val);
    if (rtpmap) media->attr[media->attr_count++] = rtpmap;

    // rtpmap: PCMU/8000
    snprintf(rtpmap_buf, sizeof(rtpmap_buf), "0 PCMU/8000");
    rtpmap_val = pj_str(rtpmap_buf);
    rtpmap = pjmedia_sdp_attr_create(pool, "rtpmap", &rtpmap_val);
    if (rtpmap) media->attr[media->attr_count++] = rtpmap;

    // sendrecv attribute
    pjmedia_sdp_attr* sendrecv = createSendRecvAttr(pool);
    if (sendrecv) media->attr[media->attr_count++] = sendrecv;

    // [RFC 5761] Mirror the peer's a=rtcp-mux offer: only repeat it when the
    // remote asked for it, otherwise the peer would send RTCP to our RTP port
    // while we still expect it on port + 1.
    if (rtcpMux) {
        pjmedia_sdp_attr* mux = pjmedia_sdp_attr_create(pool, "rtcp-mux", nullptr);
        if (mux) media->attr[media->attr_count++] = mux;
    }

    // Append to SDP
    sdp->media[sdp->media_count++] = media;

    VCS_GATEWAY_LOG_INFO("[ExternalSdp] Added audio media: port=%d, rtcp-mux=%d",
                         port, rtcpMux ? 1 : 0);
}

void ExternalSdpBuilder::addVideoMedia(pjmedia_sdp_session* sdp,
                                      pj_pool_t* pool,
                                      const std::string& localIp,
                                      uint16_t port,
                                      const RemoteMediaInfo* remoteInfo) {

    if (sdp->media_count >= PJMEDIA_MAX_SDP_MEDIA) {
        VCS_GATEWAY_LOG_WARNING("[ExternalSdp] Max media lines reached");
        return;
    }

    pjmedia_sdp_media* media = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_media);

    // m=video <port> RTP/AVP <payload_types>
    media->desc.media = pj_str((char*)"video");
    media->desc.port = port;
    media->desc.port_count = 1;
    media->desc.transport = pj_str((char*)"RTP/AVP");
    media->desc.fmt_count = 1;
    media->desc.fmt[0] = pj_str((char*)"97");  // H264

    // Connection information (media-level c= line)
    media->conn = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_conn);
    addConnectionInfo(media->conn, pool, localIp);

    // rtpmap: H264/90000
    char rtpmap_buf[64];
    snprintf(rtpmap_buf, sizeof(rtpmap_buf), "97 H264/90000");
    pj_str_t rtpmap_val = pj_str(rtpmap_buf);
    pjmedia_sdp_attr* rtpmap = pjmedia_sdp_attr_create(pool, "rtpmap", &rtpmap_val);
    if (rtpmap) media->attr[media->attr_count++] = rtpmap;

    // [Key Fix] Build H.264 fmtp parameters compatible with the remote device.
    // Use the parameters provided by the remote (if any), otherwise use defaults.
    std::string profileLevelId = "42e01f";
    std::string maxMbps;
    std::string maxFs;
    bool packetizationMode = true;

    if (remoteInfo) {
        // Use H.264 parameters parsed from the remote SDP
        if (!remoteInfo->h264ProfileLevelId.empty()) {
            profileLevelId = remoteInfo->h264ProfileLevelId;
        }
        maxMbps = remoteInfo->h264MaxMbps;
        maxFs = remoteInfo->h264MaxFs;
        packetizationMode = remoteInfo->h264PacketizationMode;
    }

    // Build the complete fmtp string.
    // Format: "97 profile-level-id=XXXXXX; max-mbps=XXXXX; max-fs=XXXX; packetization-mode=1"
    char fmtp_buf[256];
    int fmtp_len = snprintf(fmtp_buf, sizeof(fmtp_buf), "97 profile-level-id=%s", profileLevelId.c_str());

    if (!maxMbps.empty()) {
        fmtp_len += snprintf(fmtp_buf + fmtp_len, sizeof(fmtp_buf) - fmtp_len, "; max-mbps=%s", maxMbps.c_str());
    }

    if (!maxFs.empty()) {
        fmtp_len += snprintf(fmtp_buf + fmtp_len, sizeof(fmtp_buf) - fmtp_len, "; max-fs=%s", maxFs.c_str());
    }

    fmtp_len += snprintf(fmtp_buf + fmtp_len, sizeof(fmtp_buf) - fmtp_len, "; packetization-mode=%d", packetizationMode ? 1 : 0);

    VCS_GATEWAY_LOG_INFO("[ExternalSdp] Building video fmtp: %s", fmtp_buf);

    pj_str_t fmtp_val = pj_str(fmtp_buf);
    pjmedia_sdp_attr* fmtp = pjmedia_sdp_attr_create(pool, "fmtp", &fmtp_val);
    if (fmtp) media->attr[media->attr_count++] = fmtp;

    // [2026-11-01] 必须声明关键帧请求能力：RFC 4585/5104 的 rtcp-fb 是
    // 协商项，未声明的反馈对端可以合法忽略。此前 offer 里一条都没有，
    // MCU 因此不响应我们的 FIR，入会只能等对端自发 IDR；对端只发
    // PPS+IDR（无 SPS）时解码器永远起不来，表现为入会黑屏。
    // 与 Yealink 的 offer对齐：ccm fir（FIR）+ nack pli（PLI）。
    {
        pj_str_t fbFir = pj_str((char*)"97 ccm fir");
        pjmedia_sdp_attr* attrFir = pjmedia_sdp_attr_create(pool, "rtcp-fb", &fbFir);
        if (attrFir) media->attr[media->attr_count++] = attrFir;

        pj_str_t fbPli = pj_str((char*)"97 nack pli");
        pjmedia_sdp_attr* attrPli = pjmedia_sdp_attr_create(pool, "rtcp-fb", &fbPli);
        if (attrPli) media->attr[media->attr_count++] = attrPli;
    }

    // sendrecv attribute
    pjmedia_sdp_attr* sendrecv = createSendRecvAttr(pool);
    if (sendrecv) media->attr[media->attr_count++] = sendrecv;

    // [RFC 5761] Mirror a=rtcp-mux from the remote offer (see addAudioMedia).
    if (remoteInfo && remoteInfo->videoRtcpMux) {
        pjmedia_sdp_attr* mux = pjmedia_sdp_attr_create(pool, "rtcp-mux", nullptr);
        if (mux) media->attr[media->attr_count++] = mux;
    }

    // Append to SDP
    sdp->media[sdp->media_count++] = media;

    VCS_GATEWAY_LOG_INFO("[ExternalSdp] Added video media: port=%d, rtcp-mux=%d",
                         port, (remoteInfo && remoteInfo->videoRtcpMux) ? 1 : 0);
}

void ExternalSdpBuilder::addConnectionInfo(pjmedia_sdp_conn* conn,
                                         pj_pool_t* pool,
                                         const std::string& ip) {
    PJ_UNUSED_ARG(pool);
    conn->net_type = pj_str((char*)"IN");
    conn->addr_type = pj_str((char*)"IP4");
    conn->addr = pj_str((char*)ip.c_str());
}

pjmedia_sdp_attr* ExternalSdpBuilder::createSendRecvAttr(pj_pool_t* pool) {
    return pjmedia_sdp_attr_create(pool, "sendrecv", NULL);
}

}} // namespace vcs::gateway
