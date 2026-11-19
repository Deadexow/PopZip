#pragma once

#include <windows.h>

#include <string>

// 返回 true 时 newName 为用户输入的新文件名（不含目录）。
bool ShowRenameDialog(HWND owner, const std::wstring& oldName,
                      std::wstring& newName);