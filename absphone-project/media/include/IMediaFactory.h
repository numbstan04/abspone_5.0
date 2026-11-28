#pragma once

#include "media_export.h"
#include "IModuleFactory.h"
#include "IMediaControl.h"
#include "IMediaSdpNegotiator.h"
#include "Response.h"
#include <memory>

namespace vcs {
namespace media {

/**
 * @brief Media module factory interface
 *
 * Provides the creation method for the media controller.
 * Following the modular design principle, absphone uses this interface
 * to create the media controller without depending on the implementation
 * class directly.
 */
class IMediaFactory : public common::IModuleFactory {
public:
    virtual ~IMediaFactory() = default;

    /**
     * @brief Create a media controller
     * @return Pointer to the media controller interface
     */
    virtual std::unique_ptr<IMediaControl> CreateMediaControl() = 0;

    /**
     * @brief Create the application-side SDP negotiator
     *
     * Used when pjsua runs with AccountConfig::callConfig.mediaAppManaged:
     * the protocol stack no longer generates or validates SDP, so the
     * application asks this object to build offers/answers and to validate
     * SDP text. Lifetime is shared with the gateway that consumes it.
     *
     * @return Shared negotiator, never null
     */
    virtual std::shared_ptr<vcs::media_config::IMediaSdpNegotiator>
        CreateSdpNegotiator() = 0;
};

} // namespace media
} // namespace vcs
