#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <exception>
#include <filesystem>


#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <deque>
#include <vector>


#include "compress_dialog.h"
#include "compress_engine.h"
#include "conflict_dialog.h"
#include "password_dialog.h"
#include "preview_dialog.h"
#include "settings_dialog.h"
#include "sevenzip_engine.h"
#include "sky.h"
#include "theme.h"
#include "zpaq_engine.h"


namespace fs = std::filesystem;

bool g_closeAfterExtract = true;
bool g_previewAutoPlay = false;
CompressFormat g_defaultCompressFormat = CompressFormat::Zip;
int g_theme = 0;
HICON g_appIcon = nullptr;

int g_windowX = -1;
int g_windowY = -1;
int g_windowWidth = 0;
int g_windowHeight = 0;

COLORREF g_bgColor = 0;
COLORREF g_titleBarColor = 0;
COLORREF g_panelColor = 0;
COLORREF g_panelDeepColor = 0;
COLORREF g_textColor = 0;
COLORREF g_textSecondaryColor = 0;
COLORREF g_textMutedColor = 0;
COLORREF g_borderColor = 0;
COLORREF g_buttonColor = 0;
COLORREF g_buttonHoverColor = 0;
COLORREF g_accentColor = 0;
COLORREF g_accentHoverColor = 0;
COLORREF g_successColor = 0;
COLORREF g_dangerColor = 0;
COLORREF g_footerColor = 0;
COLORREF g_footerLineColor = 0;

void ApplyThemeColors() {
    const ThemePalette palette = GetThemePalette(g_theme);
    g_bgColor = palette.background;
    g_titleBarColor = palette.titleBar;
    g_panelColor = palette.panel;
    g_panelDeepColor = palette.panelDeep;
    g_textColor = palette.text;
    g_textSecondaryColor = palette.textSecondary;
    g_textMutedColor = palette.textMuted;
    g_borderColor = palette.border;
    g_buttonColor = palette.button;
    g_buttonHoverColor = palette.buttonHover;
    g_accentColor = palette.accent;
    g_accentHoverColor = palette.accentHover;
    g_successColor = palette.success;
    g_dangerColor = palette.danger;
    g_footerColor = palette.footer;
    g_footerLineColor = palette.footerLine;
}

namespace {

constexpr int kTitleBarHeight = 40;
constexpr int kWindowWidth = 900;
constexpr int kWindowHeight = 460;
constexpr int kMinWindowWidth = 720;
constexpr int kMinWindowHeight = 400;
constexpr int kButtonWidth = 46;
constexpr int kResizeBorder = 6;

// 版本号：以后每次交付递增这里，并同步 CMakeLists 里的 project VERSION
constexpr wchar_t kAppVersion[] = L"1.58.0";
constexpr char kBuildStamp[] = __DATE__ " " __TIME__;

constexpr UINT kMsgProgress = WM_APP + 1;
constexpr UINT kMsgDone = WM_APP + 2;

enum class UiState { Idle, Extracting, Done, Error };

UiState g_state = UiState::Idle;
int g_percent = 0;
int g_hoverButton = 0;
std::wstring g_currentFile;
std::wstring g_statusLine;
std::wstring g_targetDir;

bool g_lastCancelled = false;
bool g_isCompression = false;
fs::path g_compressOutput;
std::atomic<bool> g_pauseRequested{false};
int g_hoverAction = 0;

HFONT g_fontTitle = nullptr;
HFONT g_fontLarge = nullptr;
HFONT g_fontBody = nullptr;
HFONT g_fontSmall = nullptr;

std::mutex g_sharedMutex;
std::wstring g_sharedCurrentFile;
std::wstring g_sharedResultMessage;
std::wstring g_sharedOutputDir;
int g_sharedExtracted = 0;
int g_sharedSkipped = 0;
int g_sharedFailed = 0;
bool g_sharedCancelled = false;

bool g_working = false;
std::thread g_worker;
std::atomic<bool> g_cancelRequested{false};

// ------------------------- 通用工具 -------------------------

// 把编译时间（__DATE__ / __TIME__，ASCII）转成宽字符串
std::wstring BuildStampWide() {
    std::wstring result;
    for (const char* p = kBuildStamp; *p != '\0'; ++p) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }
    return result;
}

// 根据 ZIP 的 UTF-8 标志（bit 11）选择文件名编码：
// 有标志按 UTF-8，无标志按系统 ANSI（中文 Windows 即 GBK/CP936）
std::wstring NarrowToWide(const char* text, bool utf8Encoded) {
    if (text == nullptr || text[0] == '\0') return std::wstring();

    auto convert = [&](UINT codePage, DWORD flags) -> std::wstring {
        const int length = MultiByteToWideChar(codePage, flags, text, -1, nullptr, 0);
        if (length <= 1) {
            return std::wstring();
        }
        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(codePage, flags, text, -1, result.data(), length);
        result.resize(static_cast<size_t>(length - 1));
        return result;
    };

    std::wstring result = utf8Encoded
        ? convert(CP_UTF8, MB_ERR_INVALID_CHARS)
        : convert(CP_ACP, 0);

    if (!result.empty()) {
        return result;
    }

    // 标志位有时和实际编码不一致，失败时再尝试另一种编码
    result = utf8Encoded
        ? convert(CP_ACP, 0)
        : convert(CP_UTF8, MB_ERR_INVALID_CHARS);

    if (!result.empty()) {
        return result;
    }

    // 最后兜底：逐字节转宽字符，至少不丢掉条目
    std::wstring fallback;
    for (const char* p = text; *p != '\0'; ++p) {
        fallback.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }
    return fallback;
}

bool SafeEntryPath(const std::wstring& entryName, fs::path& relative) {
    if (entryName.empty()) return false;
    std::wstring normalized = entryName;
    std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
    if (normalized.find(L':') != std::wstring::npos) return false;
    const fs::path candidate(normalized);
    if (candidate.is_absolute() || candidate.has_root_name() || candidate.has_root_directory()) return false;
    for (const fs::path& part : candidate) {
        if (part == L"..") return false;
    }
    relative = candidate.lexically_normal();
    if (relative.empty() || *relative.begin() == L"..") return false;
    return true;
}

// 二次校验：最终路径必须仍然位于输出目录内部
bool IsInsideDirectory(const fs::path& base, const fs::path& target) {
    std::error_code baseError;
    std::error_code targetError;

    const fs::path canonicalBase = fs::weakly_canonical(base, baseError);
    const fs::path canonicalTarget = fs::weakly_canonical(target, targetError);

    if (baseError || targetError) {
        return false;
    }

    auto baseIt = canonicalBase.begin();
    auto targetIt = canonicalTarget.begin();

    for (; baseIt != canonicalBase.end(); ++baseIt, ++targetIt) {
        if (targetIt == canonicalTarget.end() || *baseIt != *targetIt) {
            return false;
        }
    }

    return true;
}

bool HasExtension(const std::wstring& path, const std::wstring& ext) {
    std::wstring extension = fs::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension == ext;
}

bool EndsWithNoCase(const std::wstring& text, const std::wstring& suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    const size_t offset = text.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::towlower(text[offset + i]) != std::towlower(suffix[i])) {
            return false;
        }
    }
    return true;
}

fs::path ComputeOutputDir(const fs::path& zipPath) {
    fs::path base = zipPath.parent_path();
    if (base.empty()) {
        std::error_code ec;
        base = fs::current_path(ec);
        if (ec) base = fs::path(L".");
    }

    const std::wstring fileName = zipPath.filename().wstring();
    std::wstring baseName;
    if (EndsWithNoCase(fileName, L".tar.gz")) {
        baseName = fileName.substr(0, fileName.size() - 7);
    } else if (EndsWithNoCase(fileName, L".tgz")) {
        baseName = fileName.substr(0, fileName.size() - 4);
    } else {
        baseName = zipPath.stem().wstring();
    }

    if (baseName.empty()) {
        baseName = L"PopZip_Output";
    }
    return base / baseName;
}

// 输出目录里是否已经有实际文件（用于失败时清理空目录树）
bool DirectoryContainsFile(const fs::path& dir) {
    std::error_code iterateEc;
    fs::recursive_directory_iterator it(dir, iterateEc);
    const fs::recursive_directory_iterator end;

    for (; !iterateEc && it != end; it.increment(iterateEc)) {
        std::error_code typeEc;
        if (it->is_regular_file(typeEc)) {
            return true;
        }
    }

    return false;
}

// 识别 .001 / .002 ... 形式的分卷
bool IsSplitVolumePath(const std::wstring& path, int* volumeNumber = nullptr) {
    const std::wstring extension = fs::path(path).extension().wstring();
    if (extension.size() != 4 || extension[0] != L'.') {
        return false;
    }

    for (size_t i = 1; i < 4; ++i) {
        if (extension[i] < L'0' || extension[i] > L'9') {
            return false;
        }
    }

    if (volumeNumber != nullptr) {
        *volumeNumber = (extension[1] - L'0') * 100 +
                        (extension[2] - L'0') * 10 +
                        (extension[3] - L'0');
    }

    return true;
}

// 去掉 .001 后缀，得到真实压缩包名（test.7z.001 -> test.7z）
std::wstring StripVolumeSuffix(const std::wstring& path) {
    const fs::path p(path);
    const fs::path parent = p.parent_path();
    const std::wstring stem = p.stem().wstring();
    return (parent / stem).wstring();
}

bool IsSupportedArchivePath(const std::wstring& path) {
    if (HasExtension(path, L".zip") ||
        HasExtension(path, L".zipx") ||
        HasExtension(path, L".7z") ||
        HasExtension(path, L".z01") ||
        HasExtension(path, L".rar") ||
        HasExtension(path, L".tar") ||
        HasExtension(path, L".tgz") ||
        HasExtension(path, L".gz") ||
        HasExtension(path, L".xz") ||
        HasExtension(path, L".txz") ||
        HasExtension(path, L".iso") ||
        HasExtension(path, L".img") ||
        HasExtension(path, L".zpaq")) {
        return true;
    }

    if (EndsWithNoCase(path, L".tar.gz") || EndsWithNoCase(path, L".tar.xz")) {
        return true;
    }

    int volumeNumber = 0;
    return IsSplitVolumePath(path, &volumeNumber) && volumeNumber == 1;
}

// 把 .001/.002/... 按顺序拼接到一个临时文件，供 miniz / 7z SDK 读取
bool JoinSplitVolumes(const std::wstring& firstVolumePath,
                      std::wstring& mergedPath,
                      std::wstring& errorMessage) {

    const fs::path first(firstVolumePath);
    const fs::path base = first.parent_path() / first.stem();

    std::error_code tempEc;
    const fs::path tempDir = fs::temp_directory_path(tempEc);
    if (tempEc) {
        errorMessage = L"无法获取临时目录。";
        return false;
    }

    std::wstring mergedExt = fs::path(first.stem()).extension().wstring();
    if (mergedExt.empty()) {
        mergedExt = L".7z";
    }
    const std::wstring tempName =
        L"PopZip_split_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64()) + mergedExt;
    const fs::path tempPath = tempDir / tempName;

    HANDLE output = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (output == INVALID_HANDLE_VALUE) {

        errorMessage = L"无法创建分卷合并临时文件。";
        return false;
    }

    std::vector<char> buffer(1 << 20);
    std::wstring loopError;
    int volumeIndex = 1;

    for (;; ++volumeIndex) {
        std::wstring suffix = L".";
        if (volumeIndex < 10) {
            suffix += L"00";
        } else if (volumeIndex < 100) {
            suffix += L"0";
        }
        suffix += std::to_wstring(volumeIndex);

        const fs::path part = base.wstring() + suffix;

        std::error_code existsEc;
        if (!fs::exists(part, existsEc)) {
            if (volumeIndex == 1) {
                loopError = L"找不到第一个分卷文件。";
            }
            break;
        }

        HANDLE input = CreateFileW(part.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input == INVALID_HANDLE_VALUE) {
            loopError = L"无法读取分卷: " + part.wstring();
            break;
        }

        bool partOk = true;

        for (;;) {
            if (g_cancelRequested.load()) {
                partOk = false;
                break;
            }

            DWORD read = 0;
            if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                partOk = false;
                break;
            }
            if (read == 0) {
                break;
            }

            DWORD totalWritten = 0;
            while (totalWritten < read) {
                DWORD written = 0;
                if (!WriteFile(output, buffer.data() + totalWritten,
                               read - totalWritten, &written, nullptr) || written == 0) {
                    partOk = false;
                    break;
                }
                totalWritten += written;
            }

            if (!partOk) {
                break;
            }
        }

        CloseHandle(input);

        if (!partOk) {
            loopError = g_cancelRequested.load() ? L"已取消解压。" : L"合并分卷失败。";
            break;
        }
    }

    CloseHandle(output);

    if (!loopError.empty()) {
        DeleteFileW(tempPath.c_str());
        errorMessage = loopError;
        return false;
    }

    mergedPath = tempPath.wstring();
    return true;
}

struct ExtractionResult {
    int extracted = 0;
    int skipped = 0;
    int failed = 0;
    bool cancelled = false;
    bool encryptedOrUnsupported = false;
    std::wstring message;
    std::wstring outputDir;
};

// ------------------------- ZIP 解压逻辑 (miniz) -------------------------

#if 0
struct FileWriteContext { HANDLE file = INVALID_HANDLE_VALUE; };

size_t WriteFileCallback(void* opaque, mz_uint64, const void* buffer, size_t size) {
    auto* context = static_cast<FileWriteContext*>(opaque);
    if (!context || context->file == INVALID_HANDLE_VALUE || g_cancelRequested.load()) return 0;
    const char* data = static_cast<const char*>(buffer);
    size_t remaining = size;
    while (remaining > 0) {
        if (g_cancelRequested.load()) return size - remaining;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1 << 20));
        DWORD written = 0;
        if (!WriteFile(context->file, data, chunk, &written, nullptr) || written == 0)
            return size - remaining;
        data += written;
        remaining -= written;
    }
    return size;
}

bool ExtractZip(const std::wstring& zipPathText, const fs::path& outputDir, ExtractionResult& result, HWND hwnd) {
    FILE* file = _wfopen(zipPathText.c_str(), L"rb");
    if (!file) { result.message = L"无法打开压缩包。"; return false; }
    _fseeki64(file, 0, SEEK_END);
    const long long archiveSize = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, file, static_cast<mz_uint64>(archiveSize), 0)) {
        fclose(file); result.message = L"不是有效的 ZIP 压缩包。"; return false;
    }
    const mz_uint total = mz_zip_reader_get_num_files(&zip);
    int lastPercent = -1;

    for (mz_uint i = 0; i < total; ++i) {
        if (g_cancelRequested.load()) { result.cancelled = true; break; }
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) { ++result.failed; continue; }
        const std::wstring entryName = NarrowToWide(
            stat.m_filename, (stat.m_bit_flag & 0x0800) != 0);
        fs::path relative;

        if ((stat.m_bit_flag & 0x0001) != 0) {
            // ZIP 通用标志 bit 0：条目已加密，miniz 不支持解密
            result.encryptedOrUnsupported = true;
            ++result.failed;
        } else if (!SafeEntryPath(entryName, relative)) {
            ++result.skipped;
        } else {
            const fs::path target = outputDir / relative;
            if (!IsInsideDirectory(outputDir, target)) {
                ++result.skipped;
            } else if (stat.m_is_directory) {
                std::error_code ec; fs::create_directories(target, ec);
                if (ec) ++result.failed;
            } else {
                std::error_code ec; fs::create_directories(target.parent_path(), ec);
                if (ec) ++result.failed;
                else {
                    HANDLE output = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (output == INVALID_HANDLE_VALUE) ++result.failed;
                    else {
                        FileWriteContext ctx{output};
                        if (mz_zip_reader_extract_to_callback(&zip, i, WriteFileCallback, &ctx, 0)) ++result.extracted;
                        else ++result.failed;
                        CloseHandle(output);
                    }
                }
            }
        }
        const int percent = static_cast<int>((static_cast<mz_uint64>(i + 1) * 100) / static_cast<mz_uint64>(total));
        if (percent != lastPercent || (i % 20) == 0 || (i + 1) == total) {
            { std::lock_guard<std::mutex> lock(g_sharedMutex); g_sharedCurrentFile = entryName; }
            PostMessageW(hwnd, kMsgProgress, static_cast<WPARAM>(percent), 0);
            lastPercent = percent;
        }
    }
    mz_zip_reader_end(&zip);
    fclose(file);
    return true;
}

// ------------------------- 7Z 解压逻辑 (LZMA SDK) -------------------------

static void* SzAlloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void SzFree(ISzAllocPtr p, void* address) { (void)p; free(address); }
static const ISzAlloc g_Alloc = { SzAlloc, SzFree };

bool Extract7z(const std::wstring& archivePath, const fs::path& outputDir, ExtractionResult& result, HWND hwnd) {
    CFileInStream archiveStream{};
    if (InFile_OpenW(&archiveStream.file, archivePath.c_str()) != 0) {
        result.message = L"无法打开 7z 文件。";
        return false;
    }

    FileInStream_CreateVTable(&archiveStream);
    archiveStream.wres = 0;

    CLookToRead2 lookStream{};
    lookStream.buf = static_cast<Byte*>(malloc(1 << 20)); // 1MB 缓冲区
    if (!lookStream.buf) {
        File_Close(&archiveStream.file);
        result.message = L"内存不足。";
        return false;
    }
    lookStream.bufSize = (1 << 20);
    lookStream.realStream = &archiveStream.vt;
    
    // 修复：按官方 struct 定义手动初始化 pos 与 size，并建立虚表
    lookStream.pos = 0;
    lookStream.size = 0;
    LookToRead2_CreateVTable(&lookStream, false);

    CrcGenerateTable();

    CSzArEx db{};
    SzArEx_Init(&db);
    SRes res = SzArEx_Open(&db, &lookStream.vt, &g_Alloc, &g_Alloc);
if (res != SZ_OK) {
        if (res == SZ_ERROR_UNSUPPORTED) {
            result.encryptedOrUnsupported = true;
        }
        SzArEx_Free(&db, &g_Alloc);
        ISzAlloc_Free(&g_Alloc, lookStream.buf);
        File_Close(&archiveStream.file);
        result.message = L"不是有效的 7z 文件或文件已损坏。";
        return false;
    }

    const UInt32 numItems = db.NumFiles;
    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = nullptr;
    size_t outBufferSize = 0;
    int lastPercent = -1;

    auto reportProgress = [&](const std::wstring& name, UInt32 index) {
        const int percent = static_cast<int>(
            (static_cast<UInt64>(index) + 1) * 100 / numItems);

        if (percent != lastPercent || (index % 20) == 0 || (index + 1) == numItems) {
            {
                std::lock_guard<std::mutex> lock(g_sharedMutex);
                g_sharedCurrentFile = name;
            }
            PostMessageW(hwnd, kMsgProgress, static_cast<WPARAM>(percent), 0);
            lastPercent = percent;
        }
    };

    for (UInt32 i = 0; i < numItems; ++i) {
        if (g_cancelRequested.load()) {
            result.cancelled = true;
            break;
        }

        // SzArEx_GetFileNameUtf16 返回的长度包含结尾 NUL
        const size_t nameLen = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        if (nameLen == 0 || nameLen > (static_cast<size_t>(1) << 20)) {
            ++result.skipped;
            reportProgress(std::wstring(), i);
            continue;
        }

        std::vector<UInt16> nameBuffer(nameLen);
        SzArEx_GetFileNameUtf16(&db, i, nameBuffer.data());

        std::wstring name;
        name.reserve(nameLen - 1);
        for (size_t k = 0; k + 1 < nameLen; ++k) {
            name.push_back(static_cast<wchar_t>(nameBuffer[k]));
        }

        fs::path relative;
        if (!SafeEntryPath(name, relative)) {
            ++result.skipped;
            reportProgress(name, i);
            continue;
        }

        const fs::path target = outputDir / relative;
        if (!IsInsideDirectory(outputDir, target)) {
            ++result.skipped;
            reportProgress(name, i);
            continue;
        }

        // 7z 目录条目必须用 SzArEx_IsDir 判断，不能依赖文件名末尾斜杠
        if (SzArEx_IsDir(&db, i) != 0) {
            std::error_code dirEc;
            fs::create_directories(target, dirEc);
            if (dirEc) {
                ++result.failed;
            }
            reportProgress(name, i);
            continue;
        }

        std::error_code parentError;
        fs::create_directories(target.parent_path(), parentError);
        if (parentError) {
            ++result.failed;
            reportProgress(name, i);
            continue;
        }

        size_t offset = 0;
        size_t outSizeProcessed = 0;
        res = SzArEx_Extract(&db, &lookStream.vt, i, &blockIndex,
                             &outBuffer, &outBufferSize, &offset, &outSizeProcessed,
                             &g_Alloc, &g_Alloc);

        if (res != SZ_OK) {
            if (res == SZ_ERROR_UNSUPPORTED) {
                result.encryptedOrUnsupported = true;
            }
            ++result.failed;
            reportProgress(name, i);
            continue;
        }

        HANDLE hFile = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            ++result.failed;
            reportProgress(name, i);
            continue;
        }

        bool writeOk = true;
        const Byte* data = (outBuffer != nullptr) ? (outBuffer + offset) : nullptr;
        size_t remaining = outSizeProcessed;

        while (remaining > 0) {
            if (g_cancelRequested.load()) {
                writeOk = false;
                break;
            }
            const DWORD chunk = static_cast<DWORD>(
                std::min<size_t>(remaining, static_cast<size_t>(1) << 20));
            DWORD written = 0;
            if (!WriteFile(hFile, data, chunk, &written, nullptr) || written == 0) {
                writeOk = false;
                break;
            }
            data += written;
            remaining -= written;
        }

        CloseHandle(hFile);

        if (writeOk) {
            ++result.extracted;
        } else {
            ++result.failed;
        }

        reportProgress(name, i);
    }

    if (outBuffer != nullptr) {
        ISzAlloc_Free(&g_Alloc, outBuffer);
    }
    SzArEx_Free(&db, &g_Alloc);
    ISzAlloc_Free(&g_Alloc, lookStream.buf);
    File_Close(&archiveStream.file);
    return true;
}

#endif // 旧 C 解压实现（已由内置 7-Zip 引擎替代）

// ------------------------- 密码检测 -------------------------


bool ArchiveNeedsPassword(const std::wstring& archivePath) {
    const fs::path path(archivePath);
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });

    if (extension == L".zip" || extension == L".zipx" || extension == L".z01") {
        // 统一交给内置引擎探测，避免 miniz 打不开大 ZIP/分卷 ZIP 时误判需要密码
        fs::path actualPath = path;
        if (extension == L".z01") {
            const fs::path zipPath = path.parent_path() / (path.stem().wstring() + L".zip");
            std::error_code ec;
            if (fs::exists(zipPath, ec)) {
                actualPath = zipPath;
            }
        }
        return SevenZipNeedsPassword(actualPath.wstring());
    }

    // .001 只是分卷片段，不能当独立 7z 探测；留给合并后的解压阶段处理
    if (extension == L".001") {
        return false;
    }

    if (extension == L".7z" || extension == L".rar") {
        return SevenZipNeedsPassword(archivePath);
    }

    if (extension == L".zpaq") {
        return ZpaqNeedsPassword(archivePath);
    }

    // .tar / .tgz / .tar.gz 本身不加密
    return false;
}

// ------------------------- 统一解压入口 -------------------------

void ExtractionThread(std::wstring archivePath, HWND hwnd, std::wstring password,
                      std::wstring outputDirOverride,
                      ConflictPolicy conflictPolicy) {
    ExtractionResult result;
    std::wstring actualArchivePath = archivePath;
    std::wstring nameSourcePath = archivePath;
    std::wstring mergedPath;
    bool ok = false;

    int volumeNumber = 0;
    if (IsSplitVolumePath(archivePath, &volumeNumber)) {
        if (volumeNumber != 1) {
            result.message = L"这是分卷文件，请拖入 .001 分卷。";
        } else {
            nameSourcePath = StripVolumeSuffix(archivePath);

            {
                std::lock_guard<std::mutex> lock(g_sharedMutex);
                g_sharedCurrentFile = L"正在合并分卷...";
            }
            PostMessageW(hwnd, kMsgProgress, 0, 0);

            std::wstring joinError;
            if (JoinSplitVolumes(archivePath, mergedPath, joinError)) {
                actualArchivePath = mergedPath;
            } else {
                result.message = joinError.empty() ? L"合并分卷失败。" : joinError;
            }
        }
    }

    const fs::path outputDir = outputDirOverride.empty()
        ? ComputeOutputDir(fs::path(nameSourcePath))
        : fs::path(outputDirOverride);
    result.outputDir = outputDir.wstring();

    if (result.message.empty()) {
        if (IsSupportedArchivePath(nameSourcePath) ||
            IsSupportedArchivePath(archivePath)) {
            ExtractNotify notify = [](const std::wstring& fileName) {
                std::lock_guard<std::mutex> lock(g_sharedMutex);
                g_sharedCurrentFile = fileName;
            };
            if (EndsWithNoCase(actualArchivePath, L".zpaq")) {
                ok = ExtractZpaqArchive(actualArchivePath, outputDir, password, hwnd,
                                        g_cancelRequested, g_pauseRequested,
                                        result.extracted, result.failed,
                                        result.message);
            } else {
                ok = ExtractWithSevenZip(actualArchivePath, outputDir, password, hwnd,
                                         g_cancelRequested, g_pauseRequested,
                                         result.extracted, result.failed,
                                         result.message, &notify, conflictPolicy);
            }
            result.cancelled = g_cancelRequested.load();
        } else {
            result.message = L"不支持的压缩格式。";
        }
    }


    if (!mergedPath.empty()) {
        DeleteFileW(mergedPath.c_str());
    }


    // 解压失败、或没有任何成功产出时，清掉本次产生的空目录树
    if ((!ok || result.failed > 0 || (result.extracted == 0 && result.skipped > 0)) &&
        !DirectoryContainsFile(outputDir)) {
        std::error_code removeEc;
        fs::remove_all(outputDir, removeEc);
    }

    {
        std::lock_guard<std::mutex> lock(g_sharedMutex);
        g_sharedResultMessage = result.message;
        g_sharedOutputDir = result.outputDir;
        g_sharedExtracted = result.extracted;
        g_sharedSkipped = result.skipped;
        g_sharedFailed = result.failed;
        g_sharedCancelled = result.cancelled;
    }

    // 成功判定只看错误计数，不能去匹配消息里的“失败”字样
    const bool success = ok && !result.cancelled && result.failed == 0;
    PostMessageW(hwnd, kMsgDone, success ? 1 : 0, 0);
}

struct QueuedExtraction {
    std::wstring archivePath;
    std::wstring password;
    std::wstring outputDir;
    bool skipConflict = false;
};

std::deque<QueuedExtraction> g_extractQueue;

void EnqueueExtraction(const std::wstring& archivePath,
                       const std::wstring& password,
                       const std::wstring& outputDir,
                       bool skipConflict = false) {
    g_extractQueue.push_back({archivePath, password, outputDir, skipConflict});
}

void StartExtraction(HWND hwnd, const std::wstring& archivePath,
                     const std::wstring* passwordOverride = nullptr,
                     const std::wstring* outputDirOverride = nullptr,
                     bool skipConflictPrompt = false) {
    if (g_working) {
        EnqueueExtraction(archivePath,
                          passwordOverride ? *passwordOverride : std::wstring(),
                          outputDirOverride ? *outputDirOverride : std::wstring(),
                          skipConflictPrompt);
        return;
    }
    if (g_worker.joinable()) g_worker.join();

    g_isCompression = false;

    std::wstring absolutePath = archivePath;
    std::error_code ec;
    const fs::path absolute = fs::absolute(fs::path(archivePath), ec);
    if (!ec) absolutePath = absolute.wstring();

    std::wstring password;
    if (passwordOverride != nullptr) {
        password = *passwordOverride;
    } else if (ArchiveNeedsPassword(absolutePath)) {
        const std::wstring archiveName = fs::path(absolutePath).filename().wstring();
        if (!PromptForPassword(hwnd, archiveName, password)) {
            g_state = UiState::Idle;
            g_currentFile.clear();
            g_targetDir.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }

    fs::path namingPath(absolutePath);
    int volumeNumber = 0;
    if (IsSplitVolumePath(absolutePath, &volumeNumber) && volumeNumber == 1) {
        namingPath = fs::path(StripVolumeSuffix(absolutePath));
    }

    std::wstring targetDir;
    if (outputDirOverride != nullptr && !outputDirOverride->empty()) {
        targetDir = *outputDirOverride;
    } else {
        targetDir = ComputeOutputDir(namingPath).wstring();
    }

    ConflictPolicy conflictPolicy = ConflictPolicy::Overwrite;
    std::error_code targetEc;
    if (!skipConflictPrompt &&
        fs::is_directory(fs::path(targetDir), targetEc)) {
        const ConflictChoice choice = ShowConflictDialog(hwnd, targetDir);
        if (choice == ConflictChoice::Cancel) {
            g_state = UiState::Idle;
            g_currentFile.clear();
            g_targetDir.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (choice == ConflictChoice::Skip) {
            conflictPolicy = ConflictPolicy::Skip;
        } else if (choice == ConflictChoice::Rename) {
            conflictPolicy = ConflictPolicy::Rename;
        }
    }

    g_cancelRequested = false;
    g_pauseRequested = false;
    g_state = UiState::Extracting;
    g_percent = 0;
    g_currentFile.clear();
    g_statusLine.clear();
    g_targetDir = targetDir;
    g_working = true;

    {
        std::lock_guard<std::mutex> lock(g_sharedMutex);
        g_sharedCurrentFile = L"正在解压...";
        g_sharedResultMessage.clear();
        g_sharedOutputDir.clear();
        g_sharedExtracted = 0;
        g_sharedSkipped = 0;
        g_sharedFailed = 0;
    }

    InvalidateRect(hwnd, nullptr, FALSE);

    try {
        g_worker = std::thread(ExtractionThread, absolutePath, hwnd, password,
                               targetDir, conflictPolicy);
    } catch (...) {
        g_working = false;
        g_state = UiState::Error;
        g_statusLine = L"无法创建解压线程。";
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void StartExtractionTo(HWND hwnd) {
    if (g_working) {
        return;
    }

    std::vector<wchar_t> buffer(64 * 1024, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    dialog.lpstrFilter =
        L"压缩包\0*.zip;*.zipx;*.7z;*.rar;*.tar;*.tgz;*.tar.gz;*.tar.xz;"
        L"*.txz;*.gz;*.xz;*.iso;*.zpaq;*.001\0所有文件\0*.*\0\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) {
        return;
    }
    const std::wstring archivePath(buffer.data());

    BROWSEINFOW info{};
    info.hwndOwner = hwnd;
    info.lpszTitle = L"选择解压到的文件夹";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&info);
    if (pidl == nullptr) {
        return;
    }
    wchar_t folder[MAX_PATH] = {};
    const bool gotFolder = SHGetPathFromIDListW(pidl, folder) != FALSE;
    CoTaskMemFree(pidl);
    if (!gotFolder || folder[0] == L'\0') {
        return;
    }

    const std::wstring targetDir(folder);
    StartExtraction(hwnd, archivePath, nullptr, &targetDir);
}

void StartNextQueuedExtraction(HWND hwnd) {
    if (g_extractQueue.empty()) {
        return;
    }
    const QueuedExtraction task = g_extractQueue.front();
    g_extractQueue.pop_front();
    StartExtraction(hwnd, task.archivePath,
                    task.password.empty() ? nullptr : &task.password,
                    task.outputDir.empty() ? nullptr : &task.outputDir,
                    task.skipConflict);
}

void CompressionThread(CompressOptions options, HWND hwnd) {
    int added = 0;
    std::wstring message;
    const bool ok = CompressItems(options, hwnd, g_cancelRequested,
                                   g_pauseRequested, added, message);

    {
        std::lock_guard<std::mutex> lock(g_sharedMutex);
        g_sharedResultMessage = message;
        g_sharedOutputDir = options.outputFile.parent_path().wstring();
        g_sharedExtracted = added;
        g_sharedSkipped = 0;
        g_sharedFailed = ok ? 0 : 1;
        g_sharedCancelled = g_cancelRequested.load();
    }

    PostMessageW(hwnd, kMsgDone,
                 (ok && !g_cancelRequested.load()) ? 1 : 0, 0);
}

void StartCompression(HWND hwnd, const std::vector<fs::path>& sources) {
    if (g_working) return;
    if (g_worker.joinable()) g_worker.join();

    CompressOptions options;
    options.sources = sources;
    if (!ShowCompressDialog(hwnd, sources, options)) {
        return;
    }

    g_isCompression = true;
    g_compressOutput = options.outputFile;
    g_cancelRequested = false;
    g_pauseRequested = false;
    g_state = UiState::Extracting;
    g_percent = 0;
    g_currentFile = L"正在压缩...";
    g_statusLine.clear();
    g_targetDir = options.outputFile.wstring();
    g_working = true;

    {
        std::lock_guard<std::mutex> lock(g_sharedMutex);
        g_sharedCurrentFile = L"正在压缩...";
        g_sharedResultMessage.clear();
        g_sharedOutputDir = options.outputFile.parent_path().wstring();
        g_sharedExtracted = 0;
        g_sharedSkipped = 0;
        g_sharedFailed = 0;
    }

    InvalidateRect(hwnd, nullptr, FALSE);

    try {
        g_worker = std::thread(CompressionThread, options, hwnd);
    } catch (...) {
        g_working = false;
        g_state = UiState::Error;
        g_statusLine = L"无法创建压缩线程。";
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

// ------------------------- GDI 绘制 & Win32 消息 -------------------------

void CreateFonts() {
    const wchar_t* family = L"Microsoft YaHei UI";
    g_fontTitle = CreateFontW(-15, 0, 0, 0, 600, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    g_fontLarge = CreateFontW(-30, 0, 0, 0, 400, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    g_fontBody = CreateFontW(-17, 0, 0, 0, 400, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    g_fontSmall = CreateFontW(-13, 0, 0, 0, 400, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
}

void DestroyFonts() {
    if (g_fontTitle) DeleteObject(g_fontTitle);
    if (g_fontLarge) DeleteObject(g_fontLarge);
    if (g_fontBody) DeleteObject(g_fontBody);
    if (g_fontSmall) DeleteObject(g_fontSmall);
}

void DrawCaptionButton(HDC dc, const RECT& rc, bool closeButton, bool hover) {
    if (hover) {
        HBRUSH brush =
            CreateSolidBrush(closeButton ? RGB(196, 43, 28) : g_buttonHoverColor);
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
    }
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 1, (hover && closeButton) ? RGB(255, 255, 255) : g_textColor);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (closeButton) {
        MoveToEx(dc, cx - 5, cy - 5, nullptr); LineTo(dc, cx + 6, cy + 6);
        MoveToEx(dc, cx - 5, cy + 6, nullptr); LineTo(dc, cx + 6, cy - 5);
    } else {
        MoveToEx(dc, cx - 5, cy + 1, nullptr); LineTo(dc, cx + 6, cy + 1);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawProgressBar(HDC dc, const RECT& rc, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const int radius = rc.bottom - rc.top;
    HBRUSH backgroundBrush = CreateSolidBrush(g_panelDeepColor);
    HRGN backgroundRegion = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius, radius);
    FillRgn(dc, backgroundRegion, backgroundBrush);
    DeleteObject(backgroundRegion);
    DeleteObject(backgroundBrush);

    if (percent <= 0) return;

    const int barWidth = rc.right - rc.left;
    int fillWidth = barWidth * percent / 100;
    if (fillWidth < radius) fillWidth = radius;
    HBRUSH foregroundBrush = CreateSolidBrush(g_accentColor);
    HRGN foregroundRegion = CreateRoundRectRgn(rc.left, rc.top, rc.left + fillWidth + 1, rc.bottom + 1, radius, radius);
    FillRgn(dc, foregroundRegion, foregroundBrush);
    DeleteObject(foregroundRegion);
    DeleteObject(foregroundBrush);
}

void FillRoundRect(HDC dc, const RECT& rc, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HRGN region = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1,
                                     radius, radius);
    FillRgn(dc, region, brush);
    DeleteObject(region);
    DeleteObject(brush);
}


RECT GetCloseAfterRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    return RECT{20, client.bottom - 44, 250, client.bottom - 4};
}

RECT GetExtractToRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int gap = 20;
    const int panelWidth = (width - gap * 4) / 3;
    const int panelTop = kTitleBarHeight + 20;
    const int panelBottom = height - 60;
    const int centerY = (panelTop + panelBottom) / 2;
    return RECT{gap + 30, centerY + 60, gap + panelWidth - 30, centerY + 96};
}

RECT GetNewCompressRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int gap = 20;
    const int panelWidth = (width - gap * 4) / 3;
    const int midLeft = gap + panelWidth + gap;
    const int midRight = midLeft + panelWidth;
    const int panelTop = kTitleBarHeight + 20;
    const int panelBottom = height - 60;
    const int centerY = (panelTop + panelBottom) / 2;
    return RECT{midLeft + 30, centerY + 60, midRight - 30, centerY + 96};
}

RECT GetPreviewRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int gap = 20;
    const int panelWidth = (width - gap * 4) / 3;
    const int rightLeft = gap + panelWidth + gap + panelWidth + gap;
    const int panelTop = kTitleBarHeight + 20;
    const int panelBottom = height - 60;
    const int centerY = (panelTop + panelBottom) / 2;
    return RECT{rightLeft + 30, centerY + 60, width - gap - 30, centerY + 96};
}

RECT GetSettingsRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    return RECT{client.right - 96, client.bottom - 42,
                client.right - 30, client.bottom - 10};
}

RECT GetBackRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    return RECT{client.right - 186, client.bottom - 42,
                client.right - 106, client.bottom - 10};
}

RECT GetPauseRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    return RECT{client.right - 260, client.bottom - 42,
                client.right - 180, client.bottom - 10};
}

RECT GetStopRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    return RECT{client.right - 170, client.bottom - 42,
                client.right - 100, client.bottom - 10};
}

void DrawBrandIcon(HDC dc, const RECT& rc) {
    if (g_appIcon != nullptr) {
        DrawIconEx(dc, rc.left, rc.top, g_appIcon, rc.right - rc.left,
                   rc.bottom - rc.top, 0, nullptr, DI_NORMAL);
        return;
    }

    FillRoundRect(dc, rc, 6, RGB(0, 120, 212));

    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(dc, pen);

    MoveToEx(dc, cx, cy - 6, nullptr);
    LineTo(dc, cx, cy + 3);
    MoveToEx(dc, cx - 4, cy - 2, nullptr);
    LineTo(dc, cx, cy + 3);
    MoveToEx(dc, cx + 4, cy - 2, nullptr);
    LineTo(dc, cx, cy + 3);
    MoveToEx(dc, cx - 6, cy + 6, nullptr);
    LineTo(dc, cx + 7, cy + 6);

    SelectObject(dc, oldPen);
    DeleteObject(pen);
}


SkyState g_sky;

void DrawThemeBackground(HDC dc, int width, int height) {
    if (g_theme >= 2) {
        EnsureSky(g_sky, width, height);
        DrawSky(dc, g_sky, width, height);
        return;
    }
    HBRUSH background = CreateSolidBrush(
        g_theme == 1 ? RGB(245, 245, 245) : RGB(32, 32, 32));
    RECT rc{0, 0, width, height};
    FillRect(dc, &rc, background);
    DeleteObject(background);
}

void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) { EndPaint(hwnd, &ps); return; }

    HDC memoryDc = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

    DrawThemeBackground(memoryDc, width, height);
    SetBkMode(memoryDc, TRANSPARENT);

    const RECT titleBand{0, 0, width, kTitleBarHeight};
    if (g_theme >= 2) {
        FillTranslucent(memoryDc, titleBand, g_bgColor, 205);
    } else {
        HBRUSH titleBrush = CreateSolidBrush(g_titleBarColor);
        FillRect(memoryDc, &titleBand, titleBrush);
        DeleteObject(titleBrush);
    }

    SelectObject(memoryDc, g_fontTitle);
    SetTextColor(memoryDc, g_textColor);
    RECT brandRect{20, (kTitleBarHeight - 22) / 2, 42, (kTitleBarHeight + 22) / 2};
    DrawBrandIcon(memoryDc, brandRect);
    RECT titleRect{52, 0, width - 2 * kButtonWidth - 12, kTitleBarHeight};
    const std::wstring titleText =
        std::wstring(L"PopZip v") + kAppVersion + L"  ·  by ChitoseVulpes";
    DrawTextW(memoryDc, titleText.c_str(), -1, &titleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT minimizeRect{width - 2 * kButtonWidth, 0, width - kButtonWidth, kTitleBarHeight};
    RECT closeRect{width - kButtonWidth, 0, width, kTitleBarHeight};
    DrawCaptionButton(memoryDc, minimizeRect, false, g_hoverButton == 1);
    DrawCaptionButton(memoryDc, closeRect, true, g_hoverButton == 2);

    HPEN linePen = CreatePen(PS_SOLID, 1, g_borderColor);
    HGDIOBJ oldPen = SelectObject(memoryDc, linePen);
    MoveToEx(memoryDc, 0, kTitleBarHeight, nullptr); LineTo(memoryDc, width, kTitleBarHeight);
    SelectObject(memoryDc, oldPen);
    DeleteObject(linePen);

    const int contentTop = kTitleBarHeight;
    const int contentHeight = height - contentTop;

    if (g_state == UiState::Idle) {
        const int gap = 20;
        const int panelWidth = (width - gap * 4) / 3;
        const RECT leftPanel{gap, contentTop + 20, gap + panelWidth, height - 60};
        const RECT midPanel{leftPanel.right + gap, contentTop + 20,
                            leftPanel.right + gap + panelWidth, height - 60};
        const RECT rightPanel{midPanel.right + gap, contentTop + 20,
                              width - gap, height - 60};

        HPEN borderPen = CreatePen(PS_DASH, 1, g_borderColor);
        HGDIOBJ oldBorderPen = SelectObject(memoryDc, borderPen);
        HGDIOBJ oldBorderBrush = SelectObject(memoryDc, GetStockObject(NULL_BRUSH));
        RoundRect(memoryDc, leftPanel.left, leftPanel.top, leftPanel.right,
                  leftPanel.bottom, 14, 14);
        RoundRect(memoryDc, midPanel.left, midPanel.top, midPanel.right,
                  midPanel.bottom, 14, 14);
        RoundRect(memoryDc, rightPanel.left, rightPanel.top, rightPanel.right,
                  rightPanel.bottom, 14, 14);
        SelectObject(memoryDc, oldBorderPen);
        SelectObject(memoryDc, oldBorderBrush);
        DeleteObject(borderPen);

        const int panelCenterY = (leftPanel.top + leftPanel.bottom) / 2;

        SelectObject(memoryDc, g_fontLarge);
        SetTextColor(memoryDc, g_textColor);
        RECT leftTitle{leftPanel.left, panelCenterY - 70, leftPanel.right,
                       panelCenterY - 28};
        DrawTextW(memoryDc, L"解压", -1, &leftTitle,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT midTitle{midPanel.left, panelCenterY - 70, midPanel.right,
                      panelCenterY - 28};
        DrawTextW(memoryDc, L"压缩", -1, &midTitle,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT rightTitle{rightPanel.left, panelCenterY - 70, rightPanel.right,
                        panelCenterY - 28};
        DrawTextW(memoryDc, L"预览", -1, &rightTitle,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memoryDc, g_fontBody);
        SetTextColor(memoryDc, g_textSecondaryColor);
        RECT leftHint{leftPanel.left + 10, panelCenterY - 20, leftPanel.right - 10,
                      panelCenterY + 8};
        DrawTextW(memoryDc, L"拖入压缩包即可解压", -1, &leftHint,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT midHint{midPanel.left + 10, panelCenterY - 20, midPanel.right - 10,
                     panelCenterY + 8};
        DrawTextW(memoryDc, L"拖入文件或文件夹即可压缩", -1, &midHint,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT rightHint{rightPanel.left + 10, panelCenterY - 20,
                       rightPanel.right - 10, panelCenterY + 8};
        DrawTextW(memoryDc, L"打开压缩包查看内容", -1, &rightHint,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memoryDc, g_fontSmall);
        SetTextColor(memoryDc, g_textMutedColor);
        RECT leftFormats{leftPanel.left + 10, panelCenterY + 10,
                         leftPanel.right - 10, panelCenterY + 34};
        DrawTextW(memoryDc, L"zip / 7z / rar / tar / gz / xz / iso / zpaq", -1,
                  &leftFormats, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT midFormats{midPanel.left + 10, panelCenterY + 10,
                        midPanel.right - 10, panelCenterY + 34};
        DrawTextW(memoryDc, L"ZIP / 7Z / TAR / TGZ / TAR.XZ / ZPAQ", -1,
                  &midFormats, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT rightFormats{rightPanel.left + 10, panelCenterY + 10,
                          rightPanel.right - 10, panelCenterY + 34};
        DrawTextW(memoryDc, L"ZIP / 7Z / RAR / ISO / 分卷", -1, &rightFormats,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT extractButton = GetExtractToRect(hwnd);
        FillRoundRect(memoryDc, extractButton, 8,
                      g_hoverAction == 12 ? g_accentHoverColor
                                          : g_accentColor);
        SelectObject(memoryDc, g_fontBody);
        SetTextColor(memoryDc, RGB(255, 255, 255));
        DrawTextW(memoryDc, L"解压到…", -1, &extractButton,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT newButton{midPanel.left + 30, panelCenterY + 60,
                       midPanel.right - 30, panelCenterY + 96};
        FillRoundRect(memoryDc, newButton, 8,
                      g_hoverAction == 13 ? g_accentHoverColor : g_accentColor);
        SelectObject(memoryDc, g_fontBody);
        SetTextColor(memoryDc, RGB(255, 255, 255));
        DrawTextW(memoryDc, L"新建压缩文件…", -1, &newButton,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT previewButton{rightPanel.left + 30, panelCenterY + 60,
                           rightPanel.right - 30, panelCenterY + 96};
        FillRoundRect(memoryDc, previewButton, 8,
                      g_hoverAction == 14 ? g_accentHoverColor : g_accentColor);
        SetTextColor(memoryDc, RGB(255, 255, 255));
        DrawTextW(memoryDc, L"打开预览…", -1, &previewButton,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (g_state == UiState::Extracting) {
        SelectObject(memoryDc, g_fontSmall);
        SetTextColor(memoryDc, g_textSecondaryColor);
        RECT labelRect{0, contentTop + 44, width, contentTop + 70};
        const wchar_t* labelText = g_pauseRequested.load()
            ? L"已暂停"
            : (g_isCompression ? L"正在压缩" : L"正在解压");
        DrawTextW(memoryDc, labelText, -1, &labelRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        std::wstring displayFile = g_currentFile;
        if (displayFile.empty()) displayFile = L"准备中...";

        SelectObject(memoryDc, g_fontBody);
        SetTextColor(memoryDc, g_textColor);
        RECT fileRect{40, contentTop + 78, width - 40, contentTop + 112};
        DrawTextW(memoryDc, displayFile.c_str(), -1, &fileRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT barRect{70, contentTop + 138, width - 70, contentTop + 150};
        DrawProgressBar(memoryDc, barRect, g_percent);

        SelectObject(memoryDc, g_fontSmall);
        SetTextColor(memoryDc, g_textSecondaryColor);
        const std::wstring percentText = std::to_wstring(g_percent) + L"%";
        RECT percentRect{70, contentTop + 160, width - 70, contentTop + 186};
        DrawTextW(memoryDc, percentText.c_str(), -1, &percentRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(memoryDc, g_textMutedColor);
        const std::wstring dirText =
            (g_isCompression ? L"输出到: " : L"解压到: ") + g_targetDir;
        RECT dirRect{40, height - 82, width - 40, height - 56};
        DrawTextW(memoryDc, dirText.c_str(), -1, &dirRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        const bool done = (g_state == UiState::Done);
        const bool cancelled = (!done && g_lastCancelled);
        SelectObject(memoryDc, g_fontLarge);
        SetTextColor(memoryDc, done ? g_successColor
                                   : (cancelled ? g_textSecondaryColor
                                                : g_dangerColor));
        RECT titleRect{0, contentTop + 70, width, contentTop + 120};
        const wchar_t* titleText = done
            ? (g_isCompression ? L"压缩完成" : L"解压完成")
            : (cancelled
                   ? (g_isCompression ? L"已终止压缩" : L"已终止解压")
                   : (g_isCompression ? L"压缩失败" : L"解压失败"));
        DrawTextW(memoryDc, titleText, -1, &titleRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memoryDc, g_fontBody);
        SetTextColor(memoryDc, g_textColor);
        RECT messageRect{40, contentTop + 130, width - 40, contentTop + 200};
        DrawTextW(memoryDc, g_statusLine.c_str(), -1, &messageRect, DT_CENTER | DT_WORDBREAK);

        if (done && !g_targetDir.empty()) {
            SelectObject(memoryDc, g_fontSmall);
            SetTextColor(memoryDc, g_textMutedColor);
            const std::wstring dirText = g_isCompression
                ? (L"输出文件: " + g_targetDir)
                : (L"输出目录: " + g_targetDir);
            RECT dirRect{30, contentTop + 208, width - 30, contentTop + 236};
            DrawTextW(memoryDc, dirText.c_str(), -1, &dirRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        SelectObject(memoryDc, g_fontSmall);
        SetTextColor(memoryDc, g_textMutedColor);
        RECT hintRect{0, height - 82, width, height - 56};
        DrawTextW(memoryDc,
                  g_isCompression ? L"继续拖入文件夹即可再次压缩"
                                  : L"继续拖入压缩包即可再次解压",
                  -1, &hintRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT footer{0, height - 48, width, height};
    if (g_theme >= 2) {
        FillTranslucent(memoryDc, footer, g_bgColor, 205);
    } else {
        HBRUSH footerBrush = CreateSolidBrush(g_footerColor);
        FillRect(memoryDc, &footer, footerBrush);
        DeleteObject(footerBrush);
    }

    HPEN footerPen = CreatePen(PS_SOLID, 1, g_footerLineColor);
    HGDIOBJ oldFooterPen = SelectObject(memoryDc, footerPen);
    MoveToEx(memoryDc, 0, height - 48, nullptr);
    LineTo(memoryDc, width, height - 48);
    SelectObject(memoryDc, oldFooterPen);
    DeleteObject(footerPen);

    RECT checkBox{26, height - 36, 46, height - 16};
    FillRoundRect(memoryDc, checkBox, 4,
                  g_closeAfterExtract ? g_accentColor : g_panelDeepColor);
    if (!g_closeAfterExtract) {
        HPEN boxPen = CreatePen(PS_SOLID, 1, g_borderColor);
        HGDIOBJ oldBoxPen = SelectObject(memoryDc, boxPen);
        HGDIOBJ oldBoxBrush = SelectObject(memoryDc, GetStockObject(NULL_BRUSH));
        RoundRect(memoryDc, checkBox.left, checkBox.top, checkBox.right,
                  checkBox.bottom, 4, 4);
        SelectObject(memoryDc, oldBoxPen);
        SelectObject(memoryDc, oldBoxBrush);
        DeleteObject(boxPen);
    } else {
        HPEN checkPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ oldCheckPen = SelectObject(memoryDc, checkPen);
        const int cx = (checkBox.left + checkBox.right) / 2;
        const int cy = (checkBox.top + checkBox.bottom) / 2;
        MoveToEx(memoryDc, cx - 5, cy, nullptr);
        LineTo(memoryDc, cx - 1, cy + 4);
        LineTo(memoryDc, cx + 6, cy - 5);
        SelectObject(memoryDc, oldCheckPen);
        DeleteObject(checkPen);
    }

    SelectObject(memoryDc, g_fontSmall);
    SetTextColor(memoryDc, g_textSecondaryColor);
    RECT toggleText{56, height - 42, 250, height - 6};
    DrawTextW(memoryDc, L"解压后关闭", -1, &toggleText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if ((g_state == UiState::Done || g_state == UiState::Error)) {
        RECT backRect = GetBackRect(hwnd);
        FillRoundRect(memoryDc, backRect, 8,
                      g_hoverAction == 16 ? g_buttonHoverColor : g_buttonColor);
        SelectObject(memoryDc, g_fontSmall);
        SetTextColor(memoryDc, g_hoverAction == 16 ? g_textColor
                                                   : g_textSecondaryColor);
        RECT backText = backRect;
        DrawTextW(memoryDc, L"返回", -1, &backText,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT settingsRect = GetSettingsRect(hwnd);
    FillRoundRect(memoryDc, settingsRect, 8,
                  g_hoverAction == 15 ? g_buttonHoverColor : g_buttonColor);
    SelectObject(memoryDc, g_fontSmall);
    SetTextColor(memoryDc, g_hoverAction == 15 ? g_textColor
                                               : g_textSecondaryColor);
    RECT settingsText = settingsRect;
    DrawTextW(memoryDc, L"设置", -1, &settingsText,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (g_state == UiState::Extracting) {
        auto drawAction = [&](const RECT& rect, const wchar_t* text,
                              COLORREF color, COLORREF hoverColor,
                              COLORREF textColor, bool hover) {
            FillRoundRect(memoryDc, rect, 8, hover ? hoverColor : color);
            SetTextColor(memoryDc, textColor);
            SelectObject(memoryDc, g_fontSmall);
            RECT copy = rect;
            DrawTextW(memoryDc, text, -1, &copy,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };

        RECT pauseRect = GetPauseRect(hwnd);
        RECT stopRect = GetStopRect(hwnd);
        drawAction(pauseRect,
                   g_pauseRequested.load() ? L"继续" : L"暂停",
                   g_buttonColor, g_buttonHoverColor,
                   g_theme == 1 ? g_textColor : RGB(255, 255, 255),
                   g_hoverAction == 1);
        drawAction(stopRect, L"终止",
                   RGB(150, 55, 50), RGB(185, 66, 58),
                   RGB(255, 255, 255), g_hoverAction == 2);
    }

    BitBlt(dc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);
    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    EndPaint(hwnd, &ps);
}

void EnableModernWindow(HWND hwnd) {
    BOOL darkMode = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode)))) {
        DwmSetWindowAttribute(hwnd, 19, &darkMode, sizeof(darkMode));
    }
    const int cornerPreference = 2;
    DwmSetWindowAttribute(hwnd, 33, &cornerPreference, sizeof(cornerPreference));
    const int nonClientRendering = 2;
    DwmSetWindowAttribute(hwnd, 2, &nonClientRendering, sizeof(nonClientRendering));
    MARGINS margins{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
}

RECT GetMinButtonRect(HWND hwnd) {
    RECT client{}; GetClientRect(hwnd, &client);
    return RECT{client.right - 2 * kButtonWidth, 0, client.right - kButtonWidth, kTitleBarHeight};
}

RECT GetCloseButtonRect(HWND hwnd) {
    RECT client{}; GetClientRect(hwnd, &client);
    return RECT{client.right - kButtonWidth, 0, client.right, kTitleBarHeight};
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info) {
    wchar_t dir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, dir) != 0) {
        const std::wstring file = std::wstring(dir) + L"PopZip_crash.log";
        HANDLE handle = CreateFileW(file.c_str(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            const unsigned long code =
                (info != nullptr && info->ExceptionRecord != nullptr)
                    ? info->ExceptionRecord->ExceptionCode
                    : 0;
            const void* address =
                (info != nullptr && info->ExceptionRecord != nullptr)
                    ? info->ExceptionRecord->ExceptionAddress
                    : nullptr;
            wchar_t line[256] = {};
            wsprintfW(line,
                      L"[%lu] PopZip crash code=0x%08lX address=%p\r\n",
                      GetTickCount(), code, address);
            DWORD written = 0;
            WriteFile(handle, line,
                      static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)),
                      &written, nullptr);
            CloseHandle(handle);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateFonts();
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, 1, 100, nullptr);
        return 0;

    case WM_TIMER:
        if (g_theme >= 2) {
            RECT client{};
            GetClientRect(hwnd, &client);
            UpdateSky(g_sky, client.right, client.bottom);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_NCCALCSIZE:
        if (wParam == TRUE) return 0;
        return DefWindowProcW(hwnd, message, wParam, lParam);

    case WM_NCACTIVATE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return TRUE;

    case WM_NCPAINT:
        return 0;

    case WM_ACTIVATE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        RECT client{}; GetClientRect(hwnd, &client);
        const bool left = pt.x < kResizeBorder;
        const bool right = pt.x >= client.right - kResizeBorder;
        const bool top = pt.y < kResizeBorder;
        const bool bottom = pt.y >= client.bottom - kResizeBorder;

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;

        RECT closeAfterRect = GetCloseAfterRect(hwnd);
        if (PtInRect(&closeAfterRect, pt)) return HTCLIENT;

        if (pt.y < kTitleBarHeight) {
            RECT minRect = GetMinButtonRect(hwnd);
            RECT closeRect = GetCloseButtonRect(hwnd);
            if (PtInRect(&minRect, pt) || PtInRect(&closeRect, pt)) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = kMinWindowWidth;
        info->ptMinTrackSize.y = kMinWindowHeight;
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int hover = 0;
        RECT minRect = GetMinButtonRect(hwnd);
        RECT closeRect = GetCloseButtonRect(hwnd);
        if (PtInRect(&closeRect, pt)) hover = 2;
        else if (PtInRect(&minRect, pt)) hover = 1;
        if (hover != g_hoverButton) {
            g_hoverButton = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        int action = 0;
        if (g_state == UiState::Idle) {
            RECT extractRect = GetExtractToRect(hwnd);
            RECT newRect = GetNewCompressRect(hwnd);
            RECT previewRect = GetPreviewRect(hwnd);
            if (PtInRect(&extractRect, pt)) {
                action = 12;
            } else if (PtInRect(&newRect, pt)) {
                action = 13;
            } else if (PtInRect(&previewRect, pt)) {
                action = 14;
            }
        }
        if ((g_state == UiState::Done || g_state == UiState::Error)) {
            RECT backRect = GetBackRect(hwnd);
            if (PtInRect(&backRect, pt)) action = 16;
        }
        RECT settingsRect = GetSettingsRect(hwnd);
        if (PtInRect(&settingsRect, pt)) {
            action = 15;
        }
        if (g_state == UiState::Extracting) {
            RECT pauseRect = GetPauseRect(hwnd);
            RECT stopRect = GetStopRect(hwnd);
            if (PtInRect(&pauseRect, pt)) action = 1;
            else if (PtInRect(&stopRect, pt)) action = 2;
        }
        if (action != g_hoverAction) {
            g_hoverAction = action;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        TRACKMOUSEEVENT track{ sizeof(track) };
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hoverButton != 0 || g_hoverAction != 0) {
            g_hoverButton = 0;
            g_hoverAction = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        if (g_state == UiState::Idle) {
            RECT extractRect = GetExtractToRect(hwnd);
            if (PtInRect(&extractRect, pt)) {
                StartExtractionTo(hwnd);
                return 0;
            }
            RECT newRect = GetNewCompressRect(hwnd);
            if (PtInRect(&newRect, pt)) {
                StartCompression(hwnd, {});
                return 0;
            }
            RECT previewRect = GetPreviewRect(hwnd);
            if (PtInRect(&previewRect, pt)) {
                OpenPreviewWindow(hwnd, L"", g_previewAutoPlay);
                return 0;
            }
        }

        if ((g_state == UiState::Done || g_state == UiState::Error)) {
            RECT backRect = GetBackRect(hwnd);
            if (PtInRect(&backRect, pt)) {
                g_cancelRequested = true;
                g_pauseRequested = false;
                if (g_worker.joinable()) g_worker.join();
                g_working = false;
                g_isCompression = false;
                g_state = UiState::Idle;
                g_percent = 0;
                g_currentFile.clear();
                g_statusLine.clear();
                g_targetDir.clear();
                g_compressOutput.clear();
                g_lastCancelled = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }

        RECT settingsRect = GetSettingsRect(hwnd);
        if (PtInRect(&settingsRect, pt)) {
            OpenSettingsWindow(hwnd);
            ApplyThemeColors();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        RECT closeAfterRect = GetCloseAfterRect(hwnd);
        RECT closeRect = GetCloseButtonRect(hwnd);
        RECT minRect = GetMinButtonRect(hwnd);
        RECT pauseRect = GetPauseRect(hwnd);
        RECT stopRect = GetStopRect(hwnd);

        if (PtInRect(&closeAfterRect, pt)) {
            g_closeAfterExtract = !g_closeAfterExtract;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g_state == UiState::Extracting && PtInRect(&pauseRect, pt)) {
            g_pauseRequested = !g_pauseRequested.load();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g_state == UiState::Extracting && PtInRect(&stopRect, pt)) {
            g_pauseRequested = false;
            g_cancelRequested = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (PtInRect(&closeRect, pt)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (PtInRect(&minRect, pt)) {
            ShowWindow(hwnd, SW_MINIMIZE);
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count == 0) { DragFinish(drop); return 0; }

        std::vector<fs::path> paths;
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0) continue;
            std::wstring item(static_cast<size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, i, item.data(), static_cast<UINT>(length + 1));
            item.resize(static_cast<size_t>(length));
            if (!item.empty()) paths.push_back(fs::path(item));
        }
        POINT dropPoint{};
        DragQueryPoint(drop, &dropPoint);
        DragFinish(drop);

        if (paths.empty()) return 0;

        std::error_code dirEc;
        const bool firstIsDir = fs::is_directory(paths[0], dirEc);
        const std::wstring first = paths[0].wstring();

        RECT client{};
        GetClientRect(hwnd, &client);
        const int gap = 20;
        const int panelWidth = (client.right - gap * 4) / 3;
        const int rightLeft = gap + panelWidth + gap + panelWidth + gap;
        const int rightRight = client.right - gap;
        const bool droppedInPreview =
            dropPoint.x >= rightLeft && dropPoint.x < rightRight;

        if (droppedInPreview && paths.size() == 1) {
            OpenPreviewWindow(hwnd, first, false);
            return 0;
        }

        if (paths.size() == 1 && !firstIsDir && IsSupportedArchivePath(first)) {
            StartExtraction(hwnd, first);
            return 0;
        }

        if (firstIsDir || !IsSupportedArchivePath(first)) {
            StartCompression(hwnd, paths);
            return 0;
        }

        g_state = UiState::Error;
        g_statusLine = L"请拖入压缩包解压，或拖入文件/文件夹进行压缩。";
        g_targetDir.clear();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case kMsgProgress:
        if (g_state == UiState::Extracting) {
            {
                std::lock_guard<std::mutex> lock(g_sharedMutex);
                g_currentFile = g_sharedCurrentFile;
            }
            g_percent = static_cast<int>(wParam);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case kMsgDone:
        if (g_worker.joinable()) g_worker.join();
        g_working = false;
        {
            std::lock_guard<std::mutex> lock(g_sharedMutex);
            g_statusLine = g_sharedResultMessage;
            g_lastCancelled = g_sharedCancelled;
            if (!g_sharedOutputDir.empty()) g_targetDir = g_sharedOutputDir;
        }
        if (g_lastCancelled) {
            g_statusLine = g_isCompression
                ? L"已终止压缩，未完成的输出文件已删除。"
                : L"已按要求终止，已经解压出的文件会保留在原处。";
        }
        if ((wParam == 0 || g_lastCancelled) && g_isCompression &&
            !g_compressOutput.empty()) {
            std::error_code removeEc;
            fs::remove(g_compressOutput, removeEc);
        }
        if (wParam != 0) {
            g_state = UiState::Done;
            g_percent = 100;
        } else {
            g_state = UiState::Error;
        }
        if (!g_extractQueue.empty()) {
            StartNextQueuedExtraction(hwnd);
            return 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);

        if (wParam != 0 && g_closeAfterExtract) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_EXITSIZEMOVE: {
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        g_windowX = rc.left;
        g_windowY = rc.top;
        g_windowWidth = rc.right - rc.left;
        g_windowHeight = rc.bottom - rc.top;
        SaveMainWindowPlacement(g_windowX, g_windowY, g_windowWidth,
                                g_windowHeight);
        return 0;
    }

    case WM_CLOSE:
        {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            g_windowX = rc.left;
            g_windowY = rc.top;
            g_windowWidth = rc.right - rc.left;
            g_windowHeight = rc.bottom - rc.top;
            SaveMainWindowPlacement(g_windowX, g_windowY, g_windowWidth,
                                    g_windowHeight);
        }
        g_cancelRequested = true;
        g_pauseRequested = false;
        if (g_worker.joinable()) g_worker.join();
        g_working = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        DragAcceptFiles(hwnd, FALSE);
        DestroyFonts();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();
    SetUnhandledExceptionFilter(CrashHandler);
    {
        HICON icon = reinterpret_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 22, 22,
            LR_DEFAULTCOLOR));
        if (icon != nullptr) {
            g_appIcon = icon;
        }
    }
    LoadSettings();
    ApplyThemeColors();
    std::wstring pendingZipPath;
    std::wstring pendingPassword;
    std::vector<std::wstring> extraArchives;
    bool previewMode = false;
    bool extractHere = false;
    bool commandLineExtract = false;
    bool compressMode = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        if (argc >= 3 && std::wstring(argv[1]) == L"-compress") {
            compressMode = true;
            pendingZipPath = argv[2];
        } else if (argc >= 2 && (std::wstring(argv[1]) == L"-extract-here" ||
                          std::wstring(argv[1]) == L"-extract-to")) {
            extractHere = (std::wstring(argv[1]) == L"-extract-here");
            commandLineExtract = true;
            if (argc >= 3) pendingZipPath = argv[2];
            if (argc >= 4) pendingPassword = argv[3];
        } else if (argc >= 2 && std::wstring(argv[1]) == L"-preview") {
            previewMode = true;
            if (argc >= 3) pendingZipPath = argv[2];
            if (argc >= 4) pendingPassword = argv[3];
        } else {
            if (argc >= 2) pendingZipPath = argv[1];
            for (int i = 2; i < argc; ++i) {
                const std::wstring arg = argv[i];
                std::error_code argEc;
                if (fs::exists(fs::path(arg), argEc)) {
                    extraArchives.push_back(arg);
                } else if (pendingPassword.empty()) {
                    pendingPassword = arg;
                }
            }
        }
        LocalFree(argv);
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = hInstance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    HICON appIcon = reinterpret_cast<HICON>(
        LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0,
                   LR_DEFAULTSIZE));
    if (appIcon == nullptr) {
        appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    windowClass.hIcon = appIcon;
    windowClass.hIconSm = appIcon;
    windowClass.lpszClassName = L"PopZipWindowClass";

    if (!RegisterClassExW(&windowClass)) return 1;

    const int startupWidth =
        (g_windowWidth >= kMinWindowWidth) ? g_windowWidth : kWindowWidth;
    const int startupHeight =
        (g_windowHeight >= kMinWindowHeight) ? g_windowHeight : kWindowHeight;
    int x = (GetSystemMetrics(SM_CXSCREEN) - startupWidth) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - startupHeight) / 2;
    if (g_windowX != -1 && g_windowY != -1) {
        RECT candidate{g_windowX, g_windowY, g_windowX + startupWidth,
                       g_windowY + startupHeight};
        RECT virtualScreen{
            GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_XVIRTUALSCREEN) +
                GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN) +
                GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        RECT overlap{};
        if (IntersectRect(&overlap, &candidate, &virtualScreen)) {
            x = g_windowX;
            y = g_windowY;
        }
    }
    const std::wstring windowTitle =
        std::wstring(L"PopZip v") + kAppVersion + L"  (" + BuildStampWide() + L")";
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName, windowTitle.c_str(),
                                WS_POPUP | WS_THICKFRAME, x, y, startupWidth, startupHeight,
                                nullptr, nullptr, hInstance, nullptr);

    if (hwnd == nullptr) return 1;

    EnableModernWindow(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (previewMode) {
        OpenPreviewWindow(hwnd, pendingZipPath, false, pendingPassword);
    } else if (compressMode && !pendingZipPath.empty()) {
        StartCompression(hwnd, {fs::path(pendingZipPath)});
    } else if (!pendingZipPath.empty()) {
        std::error_code dirEc;
        if (fs::is_directory(fs::path(pendingZipPath), dirEc)) {
            StartCompression(hwnd, {fs::path(pendingZipPath)});
        } else if (IsSupportedArchivePath(pendingZipPath)) {
            for (const std::wstring& extra : extraArchives) {
                if (IsSupportedArchivePath(extra)) {
                    EnqueueExtraction(extra, L"", L"");
                }
            }
            std::wstring hereDir;
            if (extractHere) {
                hereDir = fs::path(pendingZipPath).parent_path().wstring();
            }
            StartExtraction(hwnd, pendingZipPath,
                            pendingPassword.empty() ? nullptr : &pendingPassword,
                            extractHere ? &hereDir : nullptr,
                            commandLineExtract);
        }
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}