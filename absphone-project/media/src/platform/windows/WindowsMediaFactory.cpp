#include "WindowsMediaFactory.h"
#include "MFVideoSource.h"
#include "ScreenCaptureSource.h"
#include "WasapiAudioSource.h"
#include "GdiVideoSink.h"
#include "WasapiAudioSink.h"
#include "ILogger.h"
#include "VideoWindow.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace vcs {
	namespace media {
		namespace platform {
			namespace windows {

				namespace {

					std::string WideToUtf8(const std::wstring& ws) {
						if (ws.empty()) return {};
						int size = WideCharToMultiByte(CP_UTF8, 0, ws.data(),
							static_cast<int>(ws.size()),
							nullptr, 0, nullptr, nullptr);
						std::string out(static_cast<size_t>(size), 0);
						WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
							&out[0], size, nullptr, nullptr);
						return out;
					}

					// 极简 COM 智能指针
					template <typename T>
					class ComPtr {
					public:
						ComPtr() = default;
						~ComPtr() { Reset(); }
						ComPtr(const ComPtr&) = delete;
						ComPtr& operator=(const ComPtr&) = delete;

						T** Put() { Reset(); return &p_; }
						T*  Get() const { return p_; }
						T*  operator->() const { return p_; }
						explicit operator bool() const { return p_ != nullptr; }
						void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }

					private:
						T* p_ = nullptr;
					};

					// 进程级 MF 初始化，幂等
					void EnsureMediaFoundationStarted() {
						static std::once_flag once;
						std::call_once(once, [] {
							HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
							if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
								VCS_MEDIA_LOG_ERROR("[WinMediaFactory] CoInitializeEx failed: 0x%08X",
									static_cast<unsigned>(hr));
								return;
							}
							hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
							if (FAILED(hr)) {
								VCS_MEDIA_LOG_ERROR("[WinMediaFactory] MFStartup failed: 0x%08X",
									static_cast<unsigned>(hr));
							}
						});
					}

				} // anonymous namespace

				// ============================================================
				// Construction
				// ============================================================
				WindowsMediaFactory::WindowsMediaFactory() {
					EnsureMediaFoundationStarted();
					VCS_MEDIA_LOG_INFO("[WinMediaFactory] created");
				}

				WindowsMediaFactory::~WindowsMediaFactory() = default;

				// ============================================================
				// EnumerateVideoSources
				// ============================================================
				std::vector<VideoSourceInfo> WindowsMediaFactory::EnumerateVideoSources() {
					std::vector<VideoSourceInfo> result;

					// ---- 摄像头 ----
					ComPtr<IMFAttributes> attr;
					if (SUCCEEDED(MFCreateAttributes(attr.Put(), 1))) {
						attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
							MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

						IMFActivate** devices = nullptr;
						UINT32 count = 0;
						if (SUCCEEDED(MFEnumDeviceSources(attr.Get(), &devices, &count))) {
							for (UINT32 i = 0; i < count; ++i) {
								WCHAR name[256] = { 0 };
								UINT32 nameLen = 0;
								if (SUCCEEDED(devices[i]->GetString(
									MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
									name, 256, &nameLen))) {
									VideoSourceInfo info;
									info.id = "camera:" + std::to_string(i);
									info.type = VideoSourceType::Camera;
									info.displayName = WideToUtf8(name);
									result.push_back(std::move(info));
								}
								devices[i]->Release();
							}
							CoTaskMemFree(devices);
						}
					}

					// ---- 屏幕 ----
					struct Ctx { std::vector<VideoSourceInfo>* vec; };
					Ctx ctx{ &result };
					EnumDisplayMonitors(nullptr, nullptr,
						[](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
						auto* c = reinterpret_cast<Ctx*>(lParam);
						MONITORINFOEXW mi = {};
						mi.cbSize = sizeof(mi);
						if (GetMonitorInfoW(hMon, &mi)) {
							VideoSourceInfo info;
							info.id = "screen:" + std::to_string(c->vec->size());
							info.type = VideoSourceType::ScreenCapture;
							info.displayName = WideToUtf8(mi.szDevice);
							c->vec->push_back(std::move(info));
						}
						return TRUE;
					}, reinterpret_cast<LPARAM>(&ctx));

					VCS_MEDIA_LOG_INFO("[WinMediaFactory] EnumerateVideoSources: %zu",
						result.size());
					return result;
				}

				// ============================================================
				// EnumerateAudioSources
				// ============================================================
				std::vector<std::string> WindowsMediaFactory::EnumerateAudioSources() {
					std::vector<std::string> result;

					ComPtr<IMMDeviceEnumerator> enumerator;
					if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
						CLSCTX_ALL,
						__uuidof(IMMDeviceEnumerator),
						(void**)enumerator.Put()))) {
						VCS_MEDIA_LOG_ERROR("[WinMediaFactory] CoCreateInstance(MMDeviceEnumerator) failed");
						return result;
					}

					ComPtr<IMMDeviceCollection> collection;
					if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE,
						collection.Put()))) {
						return result;
					}

					UINT count = 0;
					collection->GetCount(&count);
					for (UINT i = 0; i < count; ++i) {
						result.push_back("mic:" + std::to_string(i));
					}
					return result;
				}

				// ============================================================
				// EnumerateVideoSinks
				// ============================================================
				std::vector<VideoSinkInfo> WindowsMediaFactory::EnumerateVideoSinks() {
					std::vector<VideoSinkInfo> result;
					result.push_back({ "main_window", VideoSinkType::MainWindow, "Main Window" });
					result.push_back({ "secondary_window", VideoSinkType::SecondaryWindow,
									 "Secondary Window" });
					return result;
				}

				// ============================================================
				// EnumerateAudioSinks
				// ============================================================
				std::vector<std::string> WindowsMediaFactory::EnumerateAudioSinks() {
					std::vector<std::string> result;
					ComPtr<IMMDeviceEnumerator> enumerator;
					if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
						CLSCTX_ALL,
						__uuidof(IMMDeviceEnumerator),
						(void**)enumerator.Put()))) {
						return result;
					}
					ComPtr<IMMDeviceCollection> collection;
					if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
						collection.Put()))) {
						return result;
					}
					UINT count = 0;
					collection->GetCount(&count);
					for (UINT i = 0; i < count; ++i) {
						result.push_back("speaker:" + std::to_string(i));
					}
					return result;
				}

				// ============================================================
				// CreateVideoSource / CreateAudioSource
				// ============================================================
				std::shared_ptr<IVideoSource>
					WindowsMediaFactory::CreateVideoSource(const std::string& deviceId) {
					if (deviceId.rfind("camera:", 0) == 0) {
						int index = std::atoi(deviceId.c_str() + 7);
						return std::make_shared<MFVideoSource>(index);
					}
					if (deviceId.rfind("screen:", 0) == 0) {
						int index = std::atoi(deviceId.c_str() + 7);
						return std::make_shared<ScreenCaptureSource>(index);
					}
					VCS_MEDIA_LOG_ERROR("[WinMediaFactory] CreateVideoSource: unknown id=%s",
						deviceId.c_str());
					return nullptr;
				}

				std::shared_ptr<IAudioSource>
					WindowsMediaFactory::CreateAudioSource(const std::string& deviceId) {
					// 必须在 mic:N 之前判掉：atoi("default") == 0，否则会静默
					// 退回枚举 0 位（可能是立体声混音这类静音环回端点）。
					if (deviceId == "mic:default") {
						return std::make_shared<WasapiAudioSource>(-1);
					}
					if (deviceId.rfind("mic:", 0) == 0) {
						int index = std::atoi(deviceId.c_str() + 4);
						return std::make_shared<WasapiAudioSource>(index);
					}
					VCS_MEDIA_LOG_ERROR("[WinMediaFactory] CreateAudioSource: unknown id=%s",
						deviceId.c_str());
					return nullptr;
				}

				// ============================================================
				// CreateVideoSink / CreateAudioSink
				// ============================================================
				std::shared_ptr<IVideoSink>
					WindowsMediaFactory::CreateVideoSink(
						const std::string& sinkId,
						const vcs::media_config::VideoSinkConfig& config) {
					(void)sinkId;
					auto cfg = config;
					if (!cfg.nativeWindow) {
						int w = cfg.width > 0 ? (int)cfg.width : 1280;
						int h = cfg.height > 0 ? (int)cfg.height : 720;
						cfg.nativeWindow = (void*)CreateVideoWindow(L"MCU Video", w, h);
					}
					return std::make_shared<GdiVideoSink>(cfg);
				}

				std::shared_ptr<IAudioSink>
					WindowsMediaFactory::CreateAudioSink(
						const std::string& sinkId,
						const vcs::media_config::AudioSinkConfig& config) {
					(void)sinkId;
					return std::make_shared<WasapiAudioSink>(config);
				}

				

			}
		}

		// ============================================================
		// Platform entry
		// ============================================================
		std::shared_ptr<IPlatformMediaFactory> CreatePlatformMediaFactory() {
			return std::make_shared<platform::windows::WindowsMediaFactory>();
		}
	}
}