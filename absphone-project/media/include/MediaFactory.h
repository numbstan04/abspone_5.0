#pragma once

#include "media_export.h"
#include "IModuleFactory.h"
#include "IMediaFactory.h"
#include "IMediaControl.h"
#include <memory>

namespace vcs {
namespace media {

class MediaFactory : public IMediaFactory {
public:
    MediaFactory();
    ~MediaFactory() override;

    const std::string& GetName() const override;
    const std::string& GetVersion() const override;
    bool IsInitialized() const override;

    std::unique_ptr<IMediaControl> CreateMediaControl() override;

    std::shared_ptr<vcs::media_config::IMediaSdpNegotiator>
        CreateSdpNegotiator() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace media
} // namespace vcs
