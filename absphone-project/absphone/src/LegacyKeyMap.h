#pragma once

#include "Config.h"
#include <vector>

namespace vcs { namespace absphone {

// docs/10 §3.1.8 表 A：规范键 -> hdmeeting.ini 旧节区/旧键 + 视图变换。
// 必须在 Config::LoadFromFile 之前注入；表 B（旧文件无对应）的键不在此表内。
std::vector<vcs::base::Config::ConfigKeyAlias> BuildLegacyKeyAliases();

} // namespace absphone
} // namespace vcs
