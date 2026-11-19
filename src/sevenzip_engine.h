#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using ExtractNotify = std::function<void(const std::wstring& fileName)>;

enum class ConflictPolicy { Overwrite, Skip, Rename };

struct ArchiveEntry {
    std::wstring path;
    unsigned long long size = 0;
    bool isDir = false;
    int index = -1;
};

bool SevenZipNeedsPassword(const std::wstring& archivePath);

void ReleaseArchiveCache();

bool TestArchive(const std::wstring& archivePath,
                 const std::wstring& password,
                 std::vector<std::wstring>& failedEntries,
                 std::wstring& message,
                 const std::wstring& actualPath = std::wstring());

bool PrepareArchiveSource(const std::wstring& archivePath,
                          std::wstring& actualPath,
                          std::wstring& message);

bool ListArchiveEntries(const std::wstring& archivePath,
                        const std::wstring& password,
                        std::vector<ArchiveEntry>& entries,
                        std::wstring& message,
                        const std::wstring& actualPath = std::wstring());

bool ExtractArchiveEntry(const std::wstring& archivePath,
                         const std::wstring& password,
                         int index,
                         const std::wstring& outputFile,
                         std::wstring& message,
                         const std::wstring& actualPath = std::wstring());

bool RebuildArchiveEntries(const std::wstring& archivePath,
                           const std::wstring& password,
                           const std::vector<std::wstring>& removePaths,
                           const std::wstring& renameFrom,
                           const std::wstring& renameTo,
                           std::wstring& message);

bool AppendFilesToArchive(const std::wstring& archivePath,
                          const std::wstring& password,
                          const std::vector<std::wstring>& files,
                          std::wstring& message,
                          const std::wstring& targetPrefix = std::wstring(),
                          bool overwrite = false);

bool ExtractWithSevenZip(const std::wstring& archivePath,
                         const std::filesystem::path& outputDir,
                         const std::wstring& password,
                         HWND hwnd,
                         std::atomic<bool>& cancelRequested,
                         std::atomic<bool>& pauseRequested,
                         int& extracted,
                         int& failed,
                         std::wstring& message,
                         ExtractNotify* notify = nullptr,
                         ConflictPolicy conflictPolicy =
                             ConflictPolicy::Overwrite);