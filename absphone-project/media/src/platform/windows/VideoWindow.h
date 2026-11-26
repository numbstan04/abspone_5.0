#pragma once

#include <windows.h>
#include <string>

namespace vcs { namespace media { namespace platform { namespace windows {

// 创建一个简单的 Win32 窗口，返回 HWND；失败返回 nullptr
HWND CreateVideoWindow(const std::wstring& title, int width, int height);

}}}}