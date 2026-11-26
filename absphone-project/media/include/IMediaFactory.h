#pragma once

#include "media_export.h"
#include "IModuleFactory.h"
#include "IMediaControl.h"
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
};

} // namespace media
} // namespace vcs
