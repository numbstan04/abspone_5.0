#pragma once

#include "gateway_export.h"
#include "IModuleFactory.h"
#include "IGatewayManager.h"
#include "IProtocolAdapter.h"
#include "Response.h"
#include <memory>
#include <string>

namespace vcs {
namespace gateway {

/**
 * @brief Gateway Module Factory Interface
 *
 * Provides methods to create the gateway manager and protocol adapters.
 * Follows the modular design principle: absphone uses this interface to create
 * gateway components and does not depend directly on the implementation classes.
 */
class IGatewayFactory : public common::IModuleFactory {
public:
    virtual ~IGatewayFactory() = default;

    /**
     * @brief Create the gateway manager
     * @return Gateway manager interface pointer
     */
    virtual std::unique_ptr<IGatewayManager> CreateGatewayManager() = 0;

    /**
     * @brief Create the SIP protocol adapter
     * @param listenAddress Listen address
     * @param port Listen port
     * @return SIP adapter interface pointer
     */
    virtual std::unique_ptr<IProtocolAdapter> CreateSipAdapter(
        const std::string& listenAddress,
        uint16_t port) = 0;

    /**
     * @brief Create the H.323 protocol adapter
     * @param listenAddress Listen address
     * @param port Listen port
     * @return H.323 adapter interface pointer
     */
    virtual std::unique_ptr<IProtocolAdapter> CreateH323Adapter(
        const std::string& listenAddress,
        uint16_t port) = 0;
};

} // namespace gateway
} // namespace vcs
