#pragma once

#include "common_export.h"
#include <memory>
#include <string>

namespace vcs {
namespace common {

/**
 * @brief 模块工厂基接口
 *
 * 定义模块创建和销毁的通用方法。
 * 所有业务模块工厂接口都继承此基接口。
 *
 * 放在common基础模块的原因：
 * - common模块是所有业务模块的共享依赖
 * - 业务模块依赖common是合理的设计（上层依赖下层基础设施）
 * - absphone只依赖接口，不依赖任何实现
 */
class IModuleFactory {
public:
    virtual ~IModuleFactory() = default;

    /**
     * @brief 获取工厂名称
     */
    virtual const std::string& GetName() const = 0;

    /**
     * @brief 获取工厂版本
     */
    virtual const std::string& GetVersion() const = 0;

    /**
     * @brief 工厂是否已初始化
     */
    virtual bool IsInitialized() const = 0;
};

} // namespace common
} // namespace vcs
