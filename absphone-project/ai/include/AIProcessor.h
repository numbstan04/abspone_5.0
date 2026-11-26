#pragma once

#include "IAIProcessor.h"
#include <unordered_map>
#include <memory>

namespace vcs { namespace ai {

/**
 * @brief AI处理器实现
 */
class AIProcessor : public IAIProcessor {
public:
    AIProcessor();
    ~AIProcessor() override;

    // IAIProcessor 实现
    Response StartSpeechRecognition(
        const std::string& confId,
        const std::string& memberId) override;

    Response StopSpeechRecognition(const std::string& confId) override;

    Response EnableSmartFraming(
        const std::string& confId,
        const std::string& memberId,
        bool enable) override;

    Response TranslateText(
        const std::string& text,
        const std::string& fromLang,
        const std::string& toLang,
        std::string& result) override;

    // ============================================================
    // 【AbsPhone扩展】超分辨率
    // ============================================================
    Response SetSuperResolution(bool enable, int select) override;
    Response StopSuperResolution(int select) override;
    Response GetSuperResolutionStatus(bool& enable, int& select) override;
    Response OnSuperResolutionCompleted() override;

    // ============================================================
    // 【AbsPhone扩展】帧插值
    // ============================================================
    Response SetInterpolation(bool enable, int select, uint32_t srcFps, uint32_t dstFps) override;
    Response StopInterpolation(int select) override;
    Response GetInterpolationStatus(bool& enable, int& select) override;
    Response OnInterpolationCompleted() override;
    Response SetFrameInsertionInfo(const std::string& confId,
                                 uint32_t mixerId,
                                 const std::string& memberId) override;
    Response GetFrameInsertionInfo(const std::string& confId,
                                 uint32_t& mixerId,
                                 std::string& memberId) override;

    // ============================================================
    // 【AbsPhone扩展】声源定位
    // ============================================================
    Response EnableSoundLocation(bool enable) override;
    Response GetSoundLocationStatus(bool& enabled) override;
    Response GetSoundLocationSampleRate(int& level) override;
    Response SetSoundLocationSampleRate(int level) override;
    Response GetSoundLocationIntervalTime(int& seconds) override;
    Response SetSoundLocationIntervalTime(int seconds) override;

    // ============================================================
    // 【AbsPhone扩展】人脸辅助
    // ============================================================
    Response EnableFaceAssist(bool enable) override;
    Response GetFaceAssistStatus(bool& enabled) override;

    // ============================================================
    // 【AbsPhone扩展】AI字幕/同声传译
    // ============================================================
    Response EnableITCAISoundLocation(bool enable, int mode) override;
    Response GetITCAISoundLocationStatus(bool& enabled, int& mode) override;
    Response SetSimultInterpInfo(const void* simInfo) override;
    Response GetSimultInterpInfo(void* simInfo) override;
    Response SetSimultInterpEnable(bool enable) override;
    Response SetSimultInterpServerInfo(const std::string& serverIP,
                                    uint16_t httpPort,
                                    uint16_t udpAudioPort,
                                    uint16_t udpCmdPort,
                                    uint16_t tcpCmdPort) override;
    Response GetSimultInterpServerInfo(std::string& serverIP,
                                    uint16_t& httpPort,
                                    uint16_t& udpAudioPort,
                                    uint16_t& udpCmdPort,
                                    uint16_t& tcpCmdPort) override;

    // ============================================================
    // 【AbsPhone扩展】字幕回调
    // ============================================================
    Response OnUpdateConferenceSrctSubtitles(const std::string& text,
                                          int language,
                                          bool iseof) override;
    Response OnUpdateConferenceDstSubtitles(const std::string& text,
                                         int language,
                                         bool iseof) override;

    // ============================================================
    // 【AbsPhone扩展】语音转写回调
    // ============================================================
    Response OnSendBrowseAudio(const uint8_t* frameData,
                             uint32_t frameSize,
                             uint32_t samplerate,
                             uint32_t channels) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai
} // namespace vcs
