#include "zpaq_engine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>

#include <system_error>
#include <vector>

namespace fs = std::filesystem;

extern int zpaq_main(int argc, const char** argv);

namespace {

std::string Utf8FromWide(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

fs::path CreateTempDir() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) {
        return fs::path();
    }
    const fs::path dir = base /
        (L"PopZip_zpaq_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()));
    fs::create_directories(dir, ec);
    if (ec) {
        return fs::path();
    }
    return dir;
}

bool IsSafeRelativePath(const fs::path& relative) {
    if (relative.empty() || relative.is_absolute() ||
        relative.has_root_name() || relative.has_root_directory()) {
        return false;
    }
    for (const fs::path& part : relative) {
        if (part == L".." || part == L".") {
            return false;
        }
    }
    return true;
}

void StripDuplicateTopDirectory(const fs::path& outputDir) {
    std::error_code ec;
    if (!fs::is_directory(outputDir, ec)) {
        return;
    }

    int count = 0;
    fs::path onlyDirectory;
    for (fs::directory_iterator it(outputDir, ec), end;
         !ec && it != end; it.increment(ec)) {
        ++count;
        std::error_code typeEc;
        if (it->is_directory(typeEc)) {
            onlyDirectory = it->path();
        } else {
            return;
        }
    }

    if (count != 1 || onlyDirectory.empty()) {
        return;
    }

    std::wstring inner = onlyDirectory.filename().wstring();
    std::wstring outer = outputDir.filename().wstring();
    std::transform(inner.begin(), inner.end(), inner.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    std::transform(outer.begin(), outer.end(), outer.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (inner != outer) {
        return;
    }

    const fs::path temp =
        outputDir.parent_path() / (outputDir.filename().wstring() + L"_tmp_strip");
    fs::rename(onlyDirectory, temp, ec);
    if (ec) {
        return;
    }
    fs::remove(outputDir, ec);
    if (ec) {
        std::error_code restoreEc;
        fs::rename(temp, onlyDirectory, restoreEc);
        return;
    }
    std::error_code finalEc;
    fs::rename(temp, outputDir, finalEc);
}

int CountFiles(const fs::path& dir) {
    int count = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc)) {
            ++count;
        }
    }
    return count;
}

fs::path ResolveCopyRoot(const fs::path& tempDir, const fs::path& outputDir) {
    std::error_code ec;
    int count = 0;
    fs::path onlyDirectory;
    for (fs::directory_iterator it(tempDir, ec), end;
         !ec && it != end; it.increment(ec)) {
        ++count;
        std::error_code typeEc;
        if (it->is_directory(typeEc)) {
            onlyDirectory = it->path();
        } else {
            return tempDir;
        }
    }

    if (count != 1 || onlyDirectory.empty()) {
        return tempDir;
    }

    std::wstring inner = onlyDirectory.filename().wstring();
    std::wstring outer = outputDir.filename().wstring();
    std::transform(inner.begin(), inner.end(), inner.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    std::transform(outer.begin(), outer.end(), outer.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (inner != outer) {
        return tempDir;
    }
    return onlyDirectory;
}

} // namespace

bool ZpaqNeedsPassword(const std::wstring& archivePath) {
    HANDLE handle = CreateFileW(archivePath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    char header[4] = {};
    DWORD bytesRead = 0;
    const bool ok = ReadFile(handle, header, sizeof(header), &bytesRead, nullptr) &&
                    bytesRead == sizeof(header);
    CloseHandle(handle);
    if (!ok) {
        return false;
    }
    if (std::memcmp(header, "7kSt", 4) == 0 ||
        std::memcmp(header, "zPQ", 3) == 0) {
        return false;
    }
    return true;
}

bool ExtractZpaqArchive(const std::wstring& archivePath,
                        const fs::path& outputDir,
                        const std::wstring& password,
                        HWND hwnd,
                        std::atomic<bool>& cancelRequested,
                        std::atomic<bool>& pauseRequested,
                        int& extracted,
                        int& failed,
                        std::wstring& message) {
    extracted = 0;
    failed = 0;
    message.clear();

    if (cancelRequested.load()) {
        message = L"已取消解压。";
        return false;
    }

    const fs::path tempDir = CreateTempDir();
    if (tempDir.empty()) {
        message = L"无法创建 ZPAQ 临时解压目录。";
        return false;
    }

    const std::string archiveUtf8 = Utf8FromWide(archivePath);
    const std::string tempUtf8 = Utf8FromWide(tempDir.wstring());
    const std::string passwordUtf8 = Utf8FromWide(password);

    std::vector<const char*> args;
    args.push_back("PopZip");
    args.push_back("x");
    args.push_back(archiveUtf8.c_str());
    args.push_back("-to");
    args.push_back(tempUtf8.c_str());
    args.push_back("-force");
    args.push_back("-summary");
    args.push_back("0");
    if (!password.empty()) {
        args.push_back("-key");
        args.push_back(passwordUtf8.c_str());
    }

    if (hwnd != nullptr) {
        PostMessageW(hwnd, WM_APP + 1, 0, 0);
    }

    const int result = zpaq_main(static_cast<int>(args.size()), args.data());

    if (cancelRequested.load()) {
        std::error_code removeEc;
        fs::remove_all(tempDir, removeEc);
        message = L"已取消解压。";
        return false;
    }

    if (result != 0) {
        std::error_code removeEc;
        fs::remove_all(tempDir, removeEc);
        message = password.empty()
            ? L"解压失败：ZPAQ 归档损坏或需要密码。"
            : L"解压失败：密码错误或 ZPAQ 归档损坏。";
        return false;
    }

    std::error_code ec;
    fs::create_directories(outputDir, ec);

    int copied = 0;
    int skipped = 0;
    int copyFailed = 0;
    const fs::path copyRoot = ResolveCopyRoot(tempDir, outputDir);
    const int total = CountFiles(copyRoot);

    for (fs::recursive_directory_iterator it(copyRoot, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (cancelRequested.load()) {
            break;
        }
        while (pauseRequested.load() && !cancelRequested.load()) {
            Sleep(20);
        }
        if (cancelRequested.load()) {
            break;
        }

        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) {
            continue;
        }

        std::error_code relEc;
        const fs::path relative = fs::relative(it->path(), copyRoot, relEc);
        if (relEc || !IsSafeRelativePath(relative)) {
            ++skipped;
            continue;
        }

        const fs::path target = outputDir / relative;
        std::error_code dirEc;
        fs::create_directories(target.parent_path(), dirEc);

        std::error_code copyEc;
        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing,
                      copyEc);
        if (copyEc) {
            ++copyFailed;
        } else {
            ++copied;
        }

        if (hwnd != nullptr && total > 0) {
            const int percent = static_cast<int>(
                (static_cast<long long>(copied + copyFailed + skipped) * 100) / total);
            PostMessageW(hwnd, WM_APP + 1,
                         static_cast<WPARAM>(std::min(percent, 100)), 0);
        }
    }

    std::error_code removeEc;
    fs::remove_all(tempDir, removeEc);

    StripDuplicateTopDirectory(outputDir);

    extracted = copied;
    failed = copyFailed + skipped;
    if (cancelRequested.load()) {
        message = L"已取消解压。";
        return false;
    }
    if (copied == 0 && copyFailed == 0) {
        message = L"ZPAQ 归档中没有可解压的文件。";
        return false;
    }
    message = L"解压完成：成功 " + std::to_wstring(copied) +
              L" 个文件，失败 " + std::to_wstring(failed) + L" 个。";
    if (hwnd != nullptr) {
        PostMessageW(hwnd, WM_APP + 1, 100, 0);
    }
    return failed == 0;
}