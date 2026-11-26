#include "PlatformTime.h"

#include <boost/date_time/posix_time/posix_time.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>

namespace vcs {
	namespace base {
		namespace platform {

			namespace {

				// 日历换算交给 Boost.DateTime：localtime/gmtime 的平台差异只在
				// Boost 内部（date_time/c_time.hpp），本文件没有平台分支。
				// tm_isdst 由 Boost 置为 -1（未知），调用方只消费年月日时分秒。
				std::string FormatIsoMillis(const std::tm& t, int millis) {
					char buf[40];
					std::snprintf(buf, sizeof(buf),
						"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
						t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
						t.tm_hour, t.tm_min, t.tm_sec, millis);
					return std::string(buf);
				}

			} // anonymous namespace

			// 时钟用 std::chrono：平台实现（Win 的 GetSystemTimePreciseAsFileTime、
			// POSIX 的 clock_gettime）在标准库内部，仍是同一份源码。
			uint64_t UtcNowEpochMs() {
				using namespace std::chrono;
				return static_cast<uint64_t>(
					duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
			}

			std::string UtcNowIsoMillis() {
				const uint64_t msSinceEpoch = UtcNowEpochMs();
				const std::time_t secs = static_cast<std::time_t>(msSinceEpoch / 1000);
				const int millis = static_cast<int>(msSinceEpoch % 1000);

				const std::tm t = boost::posix_time::to_tm(
					boost::posix_time::from_time_t(secs));
				return FormatIsoMillis(t, millis);
			}

			bool LocalNow(std::tm& out) {
				try {
					out = boost::posix_time::to_tm(
						boost::posix_time::second_clock::local_time());
					return true;
				}
				catch (const std::exception&) {
					return false;
				}
			}

		}
	}
}
