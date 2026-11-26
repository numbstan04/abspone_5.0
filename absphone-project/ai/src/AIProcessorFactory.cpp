#include "AIProcessorFactory.h"
#include "AIProcessor.h"
#include <memory>

namespace vcs {
namespace ai {

class AIProcessorFactory::Impl {
public:
    std::string name = "AIProcessorFactory";
    std::string version = "1.0.0";
    bool initialized = false;
};

AIProcessorFactory::AIProcessorFactory()
    : impl_(std::make_unique<Impl>()) {
}

AIProcessorFactory::~AIProcessorFactory() = default;

const std::string& AIProcessorFactory::GetName() const {
    return impl_->name;
}

const std::string& AIProcessorFactory::GetVersion() const {
    return impl_->version;
}

bool AIProcessorFactory::IsInitialized() const {
    return impl_->initialized;
}

std::unique_ptr<IAIProcessor> AIProcessorFactory::CreateAIProcessor() {
    return std::make_unique<AIProcessor>();
}

} // namespace ai
} // namespace vcs
