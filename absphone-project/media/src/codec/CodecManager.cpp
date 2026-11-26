/**
 * @file CodecManager.cpp
 * @brief Codec unified manager implementation
 *
 * Unified code entry point; build mode is decided by CMake:
 * - When VCS_USE_CODEC_SO=ON, built as a SO and statically linked into the main program
 * - Otherwise, compiled as a static library
 *
 * Either way, business code calls CreateDefaultFactory() directly.
 */

// Cross-platform includes
#include "codec/CodecManager.h"

namespace vcs { namespace media { namespace codec {

CodecManager& CodecManager::Instance() {
    static CodecManager instance;
    return instance;
}

std::unique_ptr<ICodecFactory> CodecManager::CreateDefaultFactory() {
    return CreateDefaultCodecFactory();
}

std::unique_ptr<ICodecFactory> CodecManager::CreateHardwareFactory() {
    return CreateHardwareCodecFactory();
}

std::unique_ptr<ICodecFactory> CodecManager::CreateRemoteFactory(
    const std::string& serverAddress) {
    return CreateRemoteCodecFactory(serverAddress);
}

}}} // namespace vcs::media::codec
