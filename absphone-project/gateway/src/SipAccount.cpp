#include "SipAccount.h"
#include "SipCall.h"
#include "ILogger.h"
#include <algorithm>
#include <cctype>

namespace vcs { namespace gateway {

SipAccount::SipAccount(ISipSignalingCallback* callback, int accountId)
    : callback_(callback)
    , accountId_(accountId)
{
    accountInfo_.accountId = -1;  // Will be updated after create()
}

SipAccount::~SipAccount() {
    // Account will be cleaned up automatically inside PJSIP
}

void SipAccount::Configure(const std::string& uri,
                           const std::string& registrar,
                           const std::string& username,
                           const std::string& password)
{
    accountInfo_.uri = uri;
    accountInfo_.registrar = registrar;
    accountInfo_.username = username;
    password_ = password;

    // Save configuration for use in create()
    pendingConfig_.idUri = uri;

    // If a registration server is provided, configure SIP registration
    if (!registrar.empty()) {
        pendingConfig_.regConfig.registrarUri = registrar;
    }

    // Disable Session Timers
    pendingConfig_.callConfig.timerUse = PJSUA_SIP_TIMER_INACTIVE;
    pendingConfig_.callConfig.timerMinSESec = 90;
    pendingConfig_.callConfig.timerSessExpiresSec = 1800;

    // ============================================================
    // SRTP Configuration (set by application layer via SetSrtpConfig()).
    // Map SrtpMode to PJSIP's srtpUse value.
    // ============================================================
    int srtpUse = PJMEDIA_SRTP_DISABLED;  // Default disabled
    switch (srtpConfig_.mode) {
        case vcs::media_config::SrtpMode::Disabled:
            srtpUse = PJMEDIA_SRTP_DISABLED;
            break;
        case vcs::media_config::SrtpMode::Optional:
            srtpUse = PJMEDIA_SRTP_OPTIONAL;
            break;
        case vcs::media_config::SrtpMode::Mandatory:
            srtpUse = PJMEDIA_SRTP_MANDATORY;
            break;
    }
    pendingConfig_.mediaConfig.srtpUse = static_cast<pjmedia_srtp_use>(srtpUse);
    pendingConfig_.mediaConfig.srtpSecureSignaling = srtpConfig_.requireSecureSignaling ? 1 : 0;
    VCS_GATEWAY_LOG_INFO("[SipAccount] SRTP configured: srtpUse=%d", static_cast<int>(srtpUse));

    // Video codec priorities are set globally in SipAdapter::ConfigureVideoCodec()
    // via pjsua_vid_codec_set_priority(); PJSIP 2.17 AccountMediaConfig
    // has no vidCodecList field.

    VCS_GATEWAY_LOG_INFO("[SipAccount] Configure: uri=%s", uri.c_str());
    VCS_GATEWAY_LOG_INFO("[SipAccount] Configure: registrar=%s", registrar.c_str());
}

void SipAccount::SetSrtpConfig(const vcs::media_config::SrtpConfig& config) {
    srtpConfig_ = config;
    VCS_GATEWAY_LOG_INFO("[SipAccount] SRTP config set: mode=%d", static_cast<int>(config.mode));
}

void SipAccount::onRegState(pj::OnRegStateParam& prm) {
    if (!callback_) {
        return;
    }

    accountInfo_.registered = (prm.code == 200);
    accountInfo_.accountId = getId();

    std::string reason;
    if (prm.code == 200) {
        reason = "Registered successfully";
        VCS_GATEWAY_LOG_INFO("[SipAccount] Registration successful");
    } else if (prm.code == 0) {
        reason = "Unregistered";
        VCS_GATEWAY_LOG_INFO("[SipAccount] Unregistered");
    } else {
        reason = "Registration failed";
        VCS_GATEWAY_LOG_ERROR("[SipAccount] Registration failed: code=%d", static_cast<int>(prm.code));
    }

    callback_->OnRegistrationState(accountInfo_.accountId, prm.code, reason);

    if (prm.code == 200) {
        callback_->OnRegistrationSuccess(accountInfo_.accountId);
    } else if (prm.code != 0) {
        callback_->OnRegistrationFailure(accountInfo_.accountId, prm.code, reason);
    }
}

void SipAccount::onRegStarted(pj::OnRegStartedParam& prm) {
    if (prm.renew) {
        VCS_GATEWAY_LOG_INFO("[SipAccount] Registration in progress...");
    }
}

/**
 * Extract caller information from a SIP message string.
 * Parses the From header, e.g.:
 *   From: "Yealink VC Desktop" <sip:172.16.40.247>;tag=1025979016
 *   From: <sip:172.16.40.247>;tag=xxx
 */
static void extract_caller_info_from_msg(const std::string& wholeMsg,
                                          std::string& callerUri,
                                          std::string& callerDisplayName)
{
    // Find the From line
    std::string upperMsg = wholeMsg;
    std::transform(upperMsg.begin(), upperMsg.end(), upperMsg.begin(),
                   [](unsigned char c){ return std::toupper(c); });

    size_t fromPos = upperMsg.find("FROM:");
    if (fromPos == std::string::npos) {
        VCS_GATEWAY_LOG_WARNING("[SipAccount] No From header found in message");
        return;
    }

    // Find the start and end of the From line
    size_t lineStart = fromPos;
    size_t lineEnd = wholeMsg.find('\n', lineStart);
    if (lineEnd == std::string::npos) {
        lineEnd = wholeMsg.length();
    }

    // Extract the From line content
    std::string fromLine = wholeMsg.substr(lineStart, lineEnd - lineStart);

    // Strip the "From: " prefix
    size_t colonPos = fromLine.find(':');
    if (colonPos != std::string::npos) {
        fromLine = fromLine.substr(colonPos + 1);
        // Strip leading whitespace
        while (!fromLine.empty() && std::isspace(fromLine[0])) {
            fromLine = fromLine.substr(1);
        }
    }

    // Extract the display name (if any)
    if (!fromLine.empty() && fromLine[0] == '"') {
        size_t quoteEnd = fromLine.find('"', 1);
        if (quoteEnd != std::string::npos) {
            callerDisplayName = fromLine.substr(1, quoteEnd - 1);
            fromLine = fromLine.substr(quoteEnd + 1);
        }
    }

    // Extract the SIP URI (inside <sip:...>)
    size_t uriStart = fromLine.find("<sip:");
    if (uriStart == std::string::npos) {
        uriStart = fromLine.find("<");
    }
    if (uriStart != std::string::npos) {
        size_t uriEnd = fromLine.find(">", uriStart);
        if (uriEnd != std::string::npos) {
            callerUri = fromLine.substr(uriStart, uriEnd - uriStart + 1);
            // Trim trailing whitespace
            while (!callerUri.empty() && std::isspace(callerUri.back())) {
                callerUri.pop_back();
            }
        }
    } else {
        // If no <sip:>, take everything before the first space/semicolon
        size_t spacePos = fromLine.find(' ');
        size_t semiPos = fromLine.find(';');
        size_t endPos = fromLine.length();

        if (semiPos < endPos) endPos = semiPos;
        if (spacePos < endPos) endPos = spacePos;

        callerUri = fromLine.substr(0, endPos);
    }

    VCS_GATEWAY_LOG_INFO("[SipAccount] Extracted caller: uri=%s, display=%s",
                         callerUri.c_str(), callerDisplayName.c_str());
}

void SipAccount::onIncomingCall(pj::OnIncomingCallParam& prm) {
    if (!callback_) {
        VCS_GATEWAY_LOG_WARNING("[SipAccount] No callback, rejecting call");
        return;
    }

    int callId = prm.callId;
    int accId = getId();
VCS_GATEWAY_LOG_INFO("[SipAccount] Incoming call on account %d, callId=%d", accId, callId);

    // Extract caller info from the SIP INVITE request
    std::string callerUri;
    std::string callerDisplayName;

    // Use wholeMsg to parse the From header
    extract_caller_info_from_msg(prm.rdata.wholeMsg, callerUri, callerDisplayName);

    // If extraction failed, use default
    if (callerUri.empty()) {
        callerUri = "sip:unknown@unknown";
        VCS_GATEWAY_LOG_WARNING("[SipAccount] Failed to extract caller URI, using default");
    }

    VCS_GATEWAY_LOG_INFO("[SipAccount] Caller URI: %s", callerUri.c_str());

    // Notify the upper layer of the inbound call
    callback_->OnIncomingCall(accId, callId, callerUri, callerDisplayName);
}

}} // namespace vcs::gateway
