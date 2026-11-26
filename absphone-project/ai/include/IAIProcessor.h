#pragma once

#include "ai_export.h"
#include "Response.h"
#include <string>
#include <memory>
#include <map>

namespace vcs { namespace ai {

/**
 * @brief AI处理接口
 *
 * 提供智能处理能力：语音转写、智能取景、翻译、超分、插帧等。
 * 此接口由ai模块提供，供conference/absphone模块使用。
 */
class IAIProcessor {
public:
    virtual ~IAIProcessor() = default;

    // ============================================================
    // 语音识别
    // ============================================================
    virtual Response StartSpeechRecognition(
        const std::string& confId,
        const std::string& memberId) = 0;

    virtual Response StopSpeechRecognition(const std::string& confId) = 0;

    // ============================================================
    // 智能取景
    // ============================================================
    virtual Response EnableSmartFraming(
        const std::string& confId,
        const std::string& memberId,
        bool enable) = 0;

    // ============================================================
    // 翻译
    // ============================================================
    virtual Response TranslateText(
        const std::string& text,
        const std::string& fromLang,
        const std::string& toLang,
        std::string& result) = 0;

    // ============================================================
    // 【AbsPhone扩展】超分辨率
    // ============================================================
    virtual Response SetSuperResolution(bool enable, int select) = 0;
    virtual Response StopSuperResolution(int select) = 0;
    virtual Response GetSuperResolutionStatus(bool& enable, int& select) = 0;
    virtual Response OnSuperResolutionCompleted() = 0;

    // ============================================================
    // 【AbsPhone扩展】帧插值
    // ============================================================
    virtual Response SetInterpolation(bool enable, int select, uint32_t srcFps, uint32_t dstFps) = 0;
    virtual Response StopInterpolation(int select) = 0;
    virtual Response GetInterpolationStatus(bool& enable, int& select) = 0;
    virtual Response OnInterpolationCompleted() = 0;
    virtual Response SetFrameInsertionInfo(const std::string& confId,
                                            uint32_t mixerId,
                                            const std::string& memberId) = 0;
    virtual Response GetFrameInsertionInfo(const std::string& confId,
                                            uint32_t& mixerId,
                                            std::string& memberId) = 0;

    // ============================================================
    // 【AbsPhone扩展】声源定位
    // ============================================================
    virtual Response EnableSoundLocation(bool enable) = 0;
    virtual Response GetSoundLocationStatus(bool& enabled) = 0;
    virtual Response GetSoundLocationSampleRate(int& level) = 0;
    virtual Response SetSoundLocationSampleRate(int level) = 0;
    virtual Response GetSoundLocationIntervalTime(int& seconds) = 0;
    virtual Response SetSoundLocationIntervalTime(int seconds) = 0;

    // ============================================================
    // 【AbsPhone扩展】人脸辅助
    // ============================================================
    virtual Response EnableFaceAssist(bool enable) = 0;
    virtual Response GetFaceAssistStatus(bool& enabled) = 0;

    // ============================================================
    // 【AbsPhone扩展】AI字幕/同声传译
    // ============================================================
    virtual Response EnableITCAISoundLocation(bool enable, int mode) = 0;
    virtual Response GetITCAISoundLocationStatus(bool& enabled, int& mode) = 0;
    virtual Response SetSimultInterpInfo(const void* simInfo) = 0;
    virtual Response GetSimultInterpInfo(void* simInfo) = 0;
    virtual Response SetSimultInterpEnable(bool enable) = 0;
    virtual Response SetSimultInterpServerInfo(const std::string& serverIP,
                                                uint16_t httpPort,
                                                uint16_t udpAudioPort,
                                                uint16_t udpCmdPort,
                                                uint16_t tcpCmdPort) = 0;
    virtual Response GetSimultInterpServerInfo(std::string& serverIP,
                                                uint16_t& httpPort,
                                                uint16_t& udpAudioPort,
                                                uint16_t& udpCmdPort,
                                                uint16_t& tcpCmdPort) = 0;

    // ============================================================
    // 【AbsPhone扩展】字幕回调
    // ============================================================
    virtual Response OnUpdateConferenceSrctSubtitles(const std::string& text,
                                                      int language,
                                                      bool iseof) = 0;
    virtual Response OnUpdateConferenceDstSubtitles(const std::string& text,
                                                     int language,
                                                     bool iseof) = 0;

    // ============================================================
    // 【AbsPhone扩展】语音转写回调
    // ============================================================
    virtual Response OnSendBrowseAudio(const uint8_t* frameData,
                                        uint32_t frameSize,
                                        uint32_t samplerate,
                                        uint32_t channels) = 0;
};

} // namespace ai
} // namespace vcs
