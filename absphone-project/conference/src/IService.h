#pragma once

#include <string>

namespace vcs { namespace conference {

/**
 * @brief 服务基类接口
 *
 * 所有会议模块内部服务必须继承此接口。
 * 仅供内部使用，不对外暴露。
 *
 * 【设计原则】
 * 1. 统一生命周期管理（Initialize/Shutdown）
 * 2. 服务名称标识
 */
class IService {
public:
    virtual ~IService() = default;

    /**
     * @brief 获取服务名称
     */
    virtual const char* GetName() const = 0;

    /**
     * @brief 初始化服务
     * @return 初始化是否成功
     */
    virtual bool Initialize() = 0;

    /**
     * @brief 关闭服务
     */
    virtual void Shutdown() = 0;
};

}} // namespace vcs::conference
