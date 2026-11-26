#pragma once

/**
 * @file SipAccount.h
 * @brief SIP Account Independent Implementation
 *
 * Independent SipAccount class that handles SIP registration and inbound calls.
 * Communicates with the upper layer through the ISipSignalingCallback interface.
 */

#include "SipCallHandler.h"
#include "gateway_export.h"
#include "MediaConfig.h"
#include <memory>
#include <string>

// PJSIP always enabled
#include <pjsua2.hpp>

namespace vcs { namespace gateway {

/**
 * @brief SIP Account Handler
 *
 * Independent SIP account class, inherits from pjsua2::Account, handling:
 * 1. SIP registration / unregistration
 * 2. Inbound call reception
 *
 * Notifies the upper layer through the ISipSignalingCallback interface.
 */
class SipAccount : public pj::Account
{
public:
    /**
     * @brief Constructor
     * @param callback Callback interface pointer
     * @param accountId Account ID (-1 means PJSIP assigns automatically)
     */
    SipAccount(ISipSignalingCallback* callback, int accountId = -1);

    /**
     * @brief Destructor
     */
    ~SipAccount() override;

    /**
     * @brief Get account ID (valid only after create())
     */
    int GetAccountId() const { return accountId_; }

    /**
     * @brief Set account configuration
     * @param uri Account URI (e.g., "sip:user@domain.com")
     * @param registrar Registration server (e.g., "sip:proxy.domain.com")
     * @param username Username
     * @param password Password
     */
    void Configure(const std::string& uri,
                   const std::string& registrar = "",
                   const std::string& username = "",
                   const std::string& password = "");

    /**
     * @brief Set SRTP configuration (configured by application layer)
     *
     * This method should be called before Configure().
     *
     * @param config SRTP configuration
     */
    void SetSrtpConfig(const vcs::media_config::SrtpConfig& config);

    /**
     * @brief Get account info
     */
    const SipAccountInfo& GetInfo() const { return accountInfo_; }

    /**
     * @brief Check whether registered
     */
    bool IsRegistered() const { return accountInfo_.registered; }

    /**
     * @brief Get pending configuration (used in create())
     */
    pj::AccountConfig& GetPendingConfig() { return pendingConfig_; }

protected:
    // ========== pjsua2::Account Callbacks ==========

    /// Registration state change callback
    void onRegState(pj::OnRegStateParam& prm) override;

    /// Inbound call callback
    void onIncomingCall(pj::OnIncomingCallParam& prm) override;

    /// Registration state query callback (optional)
    void onRegStarted(pj::OnRegStartedParam& prm) override;

private:
    ISipSignalingCallback* callback_;
    SipAccountInfo accountInfo_;
    std::string password_;
    int accountId_;
    pj::AccountConfig pendingConfig_;  // Configuration applied at create()

    // SRTP configuration (set by application layer via SetSrtpConfig())
    vcs::media_config::SrtpConfig srtpConfig_;
};

}} // namespace vcs::gateway
