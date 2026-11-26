#pragma once

#include "ai_export.h"
#include "IModuleFactory.h"
#include "IAIProcessorFactory.h"
#include "IAIProcessor.h"
#include <memory>

namespace vcs {
namespace ai {

class AIProcessorFactory : public IAIProcessorFactory {
public:
    AIProcessorFactory();
    ~AIProcessorFactory() override;

    const std::string& GetName() const override;
    const std::string& GetVersion() const override;
    bool IsInitialized() const override;

    std::unique_ptr<IAIProcessor> CreateAIProcessor() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai
} // namespace vcs
