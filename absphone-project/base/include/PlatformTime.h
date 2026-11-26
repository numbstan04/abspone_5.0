#pragma once

#include "vcs_export.h"

#include <cstdint>
#include <ctime>
#include <string>

namespace vcs {
	namespace base {
		namespace platform {

			// UTC 毫秒时间戳（Unix epoch 起的毫秒数），用于日志/统计的时间标记。
			// 单份实现：std::chrono，无平台分支。
			VCS_BASE_API uint64_t UtcNowEpochMs();

			// UTC 毫秒时间戳，固定格式 "YYYY-MM-DDTHH:MM:SS.mmmZ"（docs/10 §3.2.5）。
			// 单份实现：时钟走 UtcNowEpochMs，日历换算走 Boost.DateTime，无平台分支。
			// 线程安全：无静态缓冲，返回新字符串。
			VCS_BASE_API std::string UtcNowIsoMillis();

			// 本地墙钟时间（宿主时区），tm_isdst 为 -1（未知）。
			// 单份实现：Boost.DateTime 内部按平台选择 localtime_r / localtime_s。
			VCS_BASE_API bool LocalNow(std::tm& out);

		}
	}
}
