#pragma once

#include <windows.h>

#include <string>

bool ShowConfirmDialog(HWND owner, const std::wstring& title,
                       const std::wstring& message,
                       const std::wstring& okLabel);