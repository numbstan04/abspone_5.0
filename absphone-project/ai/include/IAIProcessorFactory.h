#pragma once

#include "ai_export.h"
#include "IModuleFactory.h"
#include "IAIProcessor.h"
#include "Response.h"
#include <memory>

namespace vcs {
namespace ai {

/**
 * @brief AI模块工厂接口
 *
 * 提供AI处理器的创建方法。
 * 遵循模块化设计原则，absphone通过此接口创建AI处理器，不直接依赖实现类。
 */
class IAIProcessorFactory : public common::IModuleFactory {
public:
    virtual ~IAIProcessorFactory() = default;

    /**
     * @brief 创建AI处理器
     * @return AI处理器接口指针
     */
    virtual std::unique_ptr<IAIProcessor> CreateAIProcessor() = 0;
};

} // namespace ai
} // namespace vcs
