/**
 * @file GatewayFactory.cpp
 * @brief Gateway Factory Implementation
 *
 * Provides the creation implementation for the gateway manager and protocol adapters.
 * Reads the SIP/H.323 listen addresses and ports, as well as local media configuration, from the config.
 */

#define _GNU_SOURCE 1  // Must be before any #include

#include "GatewayFactory.h"
#include "GatewayManager.h"
#include "IConfig.h"
#include "ILogger.h"

#include "SipAdapter.h"
#include "H323Adapter.h"
#include "ListenerSpec.h"

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>

#include <exception>
#include <memory>

namespace vcs {
namespace gateway {

class GatewayFactory::Impl {
public:
    std::string name = "GatewayFactory";
    std::string version = "5.4.0";
    bool initialized = false;
};

GatewayFactory::GatewayFactory()
    : impl_(std::make_unique<Impl>()) {
}

GatewayFactory::~GatewayFactory() = default;

const std::string& GatewayFactory::GetName() const {
    return impl_->name;
}

const std::string& GatewayFactory::GetVersion() const {
    return impl_->version;
}

bool GatewayFactory::IsInitialized() const {
    return impl_->initialized;
}

// ===================================================================
// Helper Function Declarations (for auto-detection)
// ===================================================================
static std::string GetLocalIpAddress();
static uint16_t GetAvailablePort(uint16_t startPort);

std::unique_ptr<IGatewayManager> GatewayFactory::CreateGatewayManager(vcs::base::IConfig* config) {
    VCS_GATEWAY_LOG_INFO("Creating GatewayManager...");

    auto manager = std::make_unique<GatewayManager>();

    // Read SIP/H.323 listen address and port from config
    std::string sipAddress = "0.0.0.0";
    uint16_t sipPort = 5060;
    std::string h323Address = "0.0.0.0";
    uint16_t h323Port = 1720;
    std::string sipTransport;   // 旧文件 transport= 参数，仅用于日志

    // Local media address and port (used for SDP generation)
    std::string sipMediaAddress;  // Empty string; auto-detect local IP if empty
    uint16_t sipAudioPort = 0;    // 0 = auto-detect available port
    uint16_t sipVideoPort = 0;    // 0 = auto-detect available port

    if (config) {
        // 监听端点只读旧文件的复合值（docs/10 A-55/A-56），不再使用自制键。
        ListenerSpec sipSpec;
        const std::string sipRaw = config->GetString("sip.listener.raw", "");
        bool sipSpecOk = false;
        if (!sipRaw.empty()) {
            sipSpecOk = ParseListenerSpec(sipRaw, &sipSpec);
            if (sipSpecOk) {
                sipAddress = sipSpec.host;
                sipTransport = sipSpec.transport;
            } else {
                VCS_GATEWAY_LOG_WARNING("[Gateway] Cannot parse SIP listener '%s'; using defaults",
                                        sipRaw.c_str());
            }
        }
        if (sipSpecOk && sipSpec.hasPort) {
            sipPort = sipSpec.port;
        } else {
            // 旧 ConfigSipListener 的端口回退顺序：SipUDPPort，其次 SipTCPPort
            int fallback = config->GetInt("SIP Parameters.SipUDPPort", 0);
            if (fallback <= 0) {
                fallback = config->GetInt("SIP Parameters.SipTCPPort", 0);
            }
            if (fallback > 0 && fallback <= 65535) {
                sipPort = static_cast<uint16_t>(fallback);
            }
        }

        ListenerSpec h323Spec;
        const std::string h323Raw = config->GetString("h323.listener.raw", "");
        if (!h323Raw.empty()) {
            if (ParseListenerSpec(h323Raw, &h323Spec)) {
                h323Address = h323Spec.host;
                if (h323Spec.hasPort) {
                    h323Port = h323Spec.port;
                }
            } else {
                VCS_GATEWAY_LOG_WARNING("[Gateway] Cannot parse H.323 listener '%s'; using defaults",
                                        h323Raw.c_str());
            }
        }

        // Read local media config (for SDP generation)
        sipMediaAddress = config->GetString("gateway.sip.media_address", "");
        sipAudioPort = static_cast<uint16_t>(config->GetInt("gateway.sip.audio_port", 0));
        sipVideoPort = static_cast<uint16_t>(config->GetInt("gateway.sip.video_port", 0));
    }

    // [V5.5 Critical Fix] If config is empty, auto-detect local IP address
    if (sipMediaAddress.empty()) {
        sipMediaAddress = GetLocalIpAddress();
        VCS_GATEWAY_LOG_INFO("[Gateway] Auto-detected local IP address: %s", sipMediaAddress.c_str());
    }

    // [V5.5 Critical Fix] If port is 0, auto-detect an available port
    if (sipAudioPort == 0) {
        sipAudioPort = GetAvailablePort(4004);  // Start detection from 4004
        VCS_GATEWAY_LOG_INFO("[Gateway] Auto-detected audio port: %u", sipAudioPort);
    }
    if (sipVideoPort == 0) {
        sipVideoPort = GetAvailablePort(sipAudioPort + 2);  // Start detection from audio port + 2
        VCS_GATEWAY_LOG_INFO("[Gateway] Auto-detected video port: %u", sipVideoPort);
    }

    VCS_GATEWAY_LOG_INFO("Gateway config loaded: SIP=%s:%u (transport=%s), H323=%s:%u",
                        sipAddress.c_str(), sipPort,
                        sipTransport.empty() ? "*" : sipTransport.c_str(),
                        h323Address.c_str(), h323Port);
    VCS_GATEWAY_LOG_INFO("Media config: address=%s, audio=%u, video=%u",
                        sipMediaAddress.c_str(), sipAudioPort, sipVideoPort);

    // Create and register the SIP adapter (GatewayManager::RegisterAdapter
    // injects the shared SessionManager into it)
    auto sipAdapter = CreateSipAdapter(sipAddress, sipPort);
    if (sipAdapter) {
        // Configure local media
        sipAdapter->SetLocalMediaAddress(sipMediaAddress);
        sipAdapter->SetLocalMediaPorts(sipAudioPort, sipVideoPort);
        manager->RegisterAdapter(std::move(sipAdapter));
        VCS_GATEWAY_LOG_INFO("SIP adapter registered");
    } else {
        VCS_GATEWAY_LOG_ERROR("Failed to create SIP adapter");
    }

    // Create and register the H.323 adapter
    auto h323Adapter = CreateH323Adapter(h323Address, h323Port);
    if (h323Adapter) {
        // Configure local media
        h323Adapter->SetLocalMediaAddress(sipMediaAddress);
        h323Adapter->SetLocalMediaPorts(sipAudioPort, sipVideoPort);
        manager->RegisterAdapter(std::move(h323Adapter));
        VCS_GATEWAY_LOG_INFO("H.323 adapter registered");
    } else {
        VCS_GATEWAY_LOG_ERROR("Failed to create H.323 adapter");
    }

    impl_->initialized = true;
    VCS_GATEWAY_LOG_INFO("GatewayManager created successfully");

    return manager;
}

std::unique_ptr<IProtocolAdapter> GatewayFactory::CreateSipAdapter(
    const std::string& listenAddress,
    uint16_t port) {
    VCS_GATEWAY_LOG_INFO("Creating SIP adapter: %s:%u", listenAddress.c_str(), port);

    try {
        auto adapter = std::make_unique<SipAdapter>(listenAddress, port);
        VCS_GATEWAY_LOG_INFO("SIP adapter created successfully");
        return adapter;
    } catch (const std::exception& e) {
        VCS_GATEWAY_LOG_ERROR("Failed to create SIP adapter: %s", e.what());
        return nullptr;
    }
}

std::unique_ptr<IProtocolAdapter> GatewayFactory::CreateH323Adapter(
    const std::string& listenAddress,
    uint16_t port) {
    VCS_GATEWAY_LOG_INFO("Creating H.323 adapter: %s:%u", listenAddress.c_str(), port);

    try {
        auto adapter = std::make_unique<H323Adapter>(listenAddress, port);
        VCS_GATEWAY_LOG_INFO("H.323 adapter created successfully");
        return adapter;
    } catch (const std::exception& e) {
        VCS_GATEWAY_LOG_ERROR("Failed to create H.323 adapter: %s", e.what());
        return nullptr;
    }
}

// ===================================================================
// Helper Functions
// ===================================================================

/**
 * @brief Get local IP address automatically using Boost.Asio
 *
 * Detects the local IP address by connecting to an external address.
 * This ensures we get the correct IP even on multi-homed systems.
 *
 * @return Local IP address string, or "127.0.0.1" if detection fails
 */
static std::string GetLocalIpAddress() {
    try {
        // [V5.5 Fix] Use Boost.Asio for cross-platform network operations
        boost::asio::io_context ioContext;

        // Create a UDP socket to detect local IP
        boost::asio::ip::udp::socket socket(ioContext);
        socket.open(boost::asio::ip::udp::v4());

        // Connect to a public DNS server (doesn't actually send data)
        boost::asio::ip::udp::endpoint endpoint(
            boost::asio::ip::address::from_string("8.8.8.8"), 53);
        socket.connect(endpoint);

        // Get local endpoint
        boost::asio::ip::udp::endpoint localEndpoint = socket.local_endpoint();
        std::string localIp = localEndpoint.address().to_string();

        socket.close();

        VCS_GATEWAY_LOG_INFO("[Gateway] Auto-detected local IP: %s", localIp.c_str());
        return localIp;
    } catch (const std::exception& e) {
        VCS_GATEWAY_LOG_WARNING("[Gateway] Failed to auto-detect IP: %s, using 127.0.0.1", e.what());
        return "127.0.0.1";
    }
}

/**
 * @brief Find an available UDP port using Boost.Asio
 *
 * Tries to bind to ports starting from startPort until successful.
 * Returns the first available port found.
 *
 * @param startPort Starting port number to try
 * @return Available port number, or 0 if none found
 */
static uint16_t GetAvailablePort(uint16_t startPort) {
    boost::asio::io_context ioContext;
    boost::system::error_code ec;

    // Scan up to 100 ports to find an available one
    for (uint16_t port = startPort; port < startPort + 100 && port < 65535; ++port) {
        boost::asio::ip::udp::socket socket(ioContext);
        socket.open(boost::asio::ip::udp::v4(), ec);
        if (ec) continue;

        boost::asio::ip::udp::endpoint endpoint(boost::asio::ip::address_v4::any(), port);
        socket.bind(endpoint, ec);

        if (!ec) {
            socket.close();
            VCS_GATEWAY_LOG_INFO("[Gateway] Found available port: %u", port);
            return port;  // Return the port we successfully bound to
        }
    }

    // Fallback: return 0 to signal failure
    VCS_GATEWAY_LOG_WARNING("[Gateway] No available port found in range %u-%u", startPort, startPort + 99);
    return 0;
}

} // namespace gateway
} // namespace vcs
