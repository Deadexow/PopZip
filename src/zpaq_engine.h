#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>

bool ZpaqNeedsPassword(const std::wstring& archivePath);

bool ExtractZpaqArchive(const std::wstring& archivePath,
                        const std::filesystem::path& outputDir,
                        const std::wstring& password,
                        HWND hwnd,
                        std::atomic<bool>& cancelRequested,
                        std::atomic<bool>& pauseRequested,
                        int& extracted,
                        int& failed,
                        std::wstring& message);