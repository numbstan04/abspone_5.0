/**
 * @file ExternalSdpBuilder.h
 * @brief External SDP Builder
 *
 * The gateway module generates a complete SDP itself and hands it to PJSIP for negotiation.
 * PJSIP does not open any audio/video device; the media transmission is fully taken over externally.
 */

#pragma once

#include <pjsua2.hpp>
#include <pjmedia.h>
#include <pjmedia/sdp.h>
#include <string>

namespace vcs { namespace gateway {

// Forward declaration
struct RemoteMediaInfo;

/**
 * @brief External SDP Builder
 *
 * Used to generate a complete SDP and hand it to PJSIP for SIP negotiation.
 * PJSIP only handles signaling; it does not manage media devices.
 */
class ExternalSdpBuilder {
public:
    /**
     * @brief Build an Answer SDP (for Answer)
     * @param remoteInfo Remote media info (contains H.264 parameters)
     * @param localIp Local IP address
     * @param audioPort Audio RTP port
     * @param videoPort Video RTP port
     * @return The generated Answer SDP
     */
    static pj::SdpSession buildAnswerSdp(
        const RemoteMediaInfo& remoteInfo,
        const std::string& localIp,
        uint16_t audioPort = 0,   // 0 = auto-detect
        uint16_t videoPort = 0);  // 0 = auto-detect

    /**
     * @brief Build an Offer SDP (for MakeCall/Offer) - 3-parameter version
     * @param localIp Local IP address
     * @param audioPort Audio RTP port
     * @param videoPort Video RTP port
     * @return The generated Offer SDP
     */
    static pj::SdpSession buildOfferSdp(
        const std::string& localIp,
        uint16_t audioPort,
        uint16_t videoPort);

    /**
     * @brief Build an Offer SDP (for MakeCall/Offer) - 4-parameter version
     * @param localIp Local IP address
     * @param audioPort Audio RTP port
     * @param videoPort Video RTP port
     * @param remoteInfo Remote media info (contains H.264 parameters, optional)
     * @return The generated Offer SDP
     */
    static pj::SdpSession buildOfferSdp(
        const std::string& localIp,
        uint16_t audioPort,
        uint16_t videoPort,
        const RemoteMediaInfo* remoteInfo);

    /**
     * @brief Create a pj::SdpSession from a pjmedia_sdp_session
     */
    static pj::SdpSession fromPjMediaSdp(pjmedia_sdp_session* sdp);

private:
    /**
     * @brief Add an audio media line
     */
    static void addAudioMedia(pjmedia_sdp_session* sdp,
                              pj_pool_t* pool,
                              const std::string& localIp,
                              uint16_t port,
                              bool rtcpMux = false);

    /**
     * @brief Add a video media line
     * @param remoteInfo Remote media info (contains H.264 parameters, optional)
     */
    static void addVideoMedia(pjmedia_sdp_session* sdp,
                              pj_pool_t* pool,
                              const std::string& localIp,
                              uint16_t port,
                              const RemoteMediaInfo* remoteInfo = nullptr);

    /**
     * @brief Add connection information
     */
    static void addConnectionInfo(pjmedia_sdp_conn* conn,
                                 pj_pool_t* pool,
                                 const std::string& ip);

    /**
     * @brief Create a sendrecv attribute
     */
    static pjmedia_sdp_attr* createSendRecvAttr(pj_pool_t* pool);
};

}} // namespace vcs::gateway
