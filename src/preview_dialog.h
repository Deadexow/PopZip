#pragma once

#include <windows.h>

#include <string>

bool IsPreviewablePath(const std::wstring& path);

void OpenPreviewWindow(HWND owner, const std::wstring& path, bool autoPlay,
                       const std::wstring& password = std::wstring());