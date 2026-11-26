#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace vcs {
	namespace media {

		/**
		 * @brief 共享音频帧（PCM）
		 *
		 * 与 AudioPcmFrame（裸指针 + new int16_t[] 所有权约定）不同：
		 *   - data 是 shared_ptr，支持一对多分发
		 *   - 消费方拿到的是 shared_ptr<const AudioFrame>，只读
		 */
		struct AudioFrame {
			std::shared_ptr<std::vector<int16_t>> data;
			size_t samples = 0;
			uint32_t sampleRate = 8000;
			uint8_t channels = 1;
			uint64_t timestamp = 0;
		};

		using AudioFramePtr = std::shared_ptr<const AudioFrame>;

	}
} // namespace vcs::media
