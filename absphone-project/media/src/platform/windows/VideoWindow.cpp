#include "VideoWindow.h"
#include "ILogger.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace vcs { namespace media { namespace platform { namespace windows {

namespace {

	// The HWND must be owned by a thread that pumps messages. A top-level
	// window whose owner thread never pumps is reported as "Not Responding"
	// by Windows, and frames blitted into its DC from the decode thread never
	// reach the screen (the window stays black). The caller is the media
	// thread, which has no message loop, so the window lives on its own thread
	// and this thread stays alive for the window's whole lifetime.
	struct WindowHost {
		std::mutex              mu;
		std::condition_variable cv;
		HWND                    hwnd = nullptr;
		bool                    created = false;
	};
}

static LRESULT CALLBACK VideoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // 黑底
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateVideoWindow(const std::wstring& title, int width, int height) {
    auto host = std::make_shared<WindowHost>();

    std::thread([host, title, width, height]() {
        HINSTANCE hInst = GetModuleHandleW(nullptr);

        const wchar_t* kClassName = L"VCS_VideoWindow";

        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = VideoWndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = kClassName;

        RegisterClassExW(&wc);

        HWND hwnd = CreateWindowExW(
            0, kClassName, title.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height,
            nullptr, nullptr, hInst, nullptr);

        const DWORD lastError = GetLastError();

        {
            std::lock_guard<std::mutex> lock(host->mu);
            host->hwnd = hwnd;
            host->created = true;
        }
        host->cv.notify_all();

        if (!hwnd) {
            VCS_MEDIA_LOG_ERROR("[VideoWindow] CreateWindowExW failed: %lu", lastError);
            return;
        }

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        VCS_MEDIA_LOG_INFO("[VideoWindow] created: hwnd=%p on window thread", hwnd);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        VCS_MEDIA_LOG_INFO("[VideoWindow] window thread exiting: hwnd=%p", hwnd);
    }).detach();

    std::unique_lock<std::mutex> lock(host->mu);
    host->cv.wait(lock, [&host]() { return host->created; });
    return host->hwnd;
}

}}}}
