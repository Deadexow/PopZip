#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

enum class CompressFormat {
    Zip,
    SevenZip,
    Tar,
    Gz,
    Xz,
    Tgz,
    TarXz,
    Zpaq
};

struct CompressOptions {
    std::vector<std::filesystem::path> sources;
    std::filesystem::path outputFile;
    CompressFormat format = CompressFormat::Zip;
    int level = 5;
    std::wstring password;
    std::wstring method;
    int threads = 0;
    unsigned long long volumeSize = 0;
};

const wchar_t* CompressFormatExtension(CompressFormat format);

bool CompressItems(const CompressOptions& options,
                   HWND hwnd,
                   std::atomic<bool>& cancelRequested,
                   std::atomic<bool>& pauseRequested,
                   int& addedFiles,
                   std::wstring& message);