#pragma once

// _GNU_SOURCE is defined via CMakeLists.txt

#include "gateway_export.h"
#include "IModuleFactory.h"
#include "IGatewayManager.h"
#include "IProtocolAdapter.h"
#include <memory>
#include <string>

namespace vcs { namespace base {
class IConfig;
} }

namespace vcs {
namespace gateway {

/**
 * @brief Gateway module factory interface
 *
 * Provides creation methods for the gateway manager and protocol adapters.
 * Follows the two-way decoupling principle:
 * - absphone creates gateway components through this interface without depending on the implementation class
 * - The gateway module implementation can be freely refactored without affecting absphone
 */
class IGatewayFactory : public common::IModuleFactory {
public:
    virtual ~IGatewayFactory() = default;

    /**
     * @brief Create a gateway manager (loaded from configuration)
     * @param config Pointer to the configuration manager
     * @return Pointer to the gateway manager interface
     */
    virtual std::unique_ptr<IGatewayManager> CreateGatewayManager(vcs::base::IConfig* config) = 0;

    /**
     * @brief Create a SIP protocol adapter
     * @param listenAddress Listen address
     * @param port Listen port
     * @return Pointer to the SIP adapter interface
     */
    virtual std::unique_ptr<IProtocolAdapter> CreateSipAdapter(
        const std::string& listenAddress,
        uint16_t port) = 0;

    /**
     * @brief Create an H.323 protocol adapter
     * @param listenAddress Listen address
     * @param port Listen port
     * @return Pointer to the H.323 adapter interface
     */
    virtual std::unique_ptr<IProtocolAdapter> CreateH323Adapter(
        const std::string& listenAddress,
        uint16_t port) = 0;
};

/**
 * @brief Gateway module factory implementation
 *
 * Provides concrete creation implementations for the gateway manager and protocol adapters.
 */
class GatewayFactory : public IGatewayFactory {
public:
    GatewayFactory();
    ~GatewayFactory() override;

    const std::string& GetName() const override;
    const std::string& GetVersion() const override;
    bool IsInitialized() const override;

    std::unique_ptr<IGatewayManager> CreateGatewayManager(vcs::base::IConfig* config) override;
    std::unique_ptr<IProtocolAdapter> CreateSipAdapter(
        const std::string& listenAddress,
        uint16_t port) override;
    std::unique_ptr<IProtocolAdapter> CreateH323Adapter(
        const std::string& listenAddress,
        uint16_t port) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gateway
} // namespace vcs
