#pragma once

#include <windows.h>

#include <string>

bool PromptForPassword(HWND parent, const std::wstring& archiveName, std::wstring& password);