#include "sevenzip_engine.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <set>

#include <string>
#include <vector>

#include "Common/MyWindows.h"

#include "Common/MyCom.h"
#include "7zip/Archive/IArchive.h"
#include "7zip/IPassword.h"
#include "7zip/IProgress.h"
#include "7zip/PropID.h"
#include <propidl.h>
#include <oleauto.h>

#include "compress_engine.h"

namespace fs = std::filesystem;

extern "C" HRESULT __stdcall CreateObject(const GUID* clsid, const GUID* iid, void** outObject);

namespace {


// 7-Zip 完整格式 GUID：{23170F69-40C1-278A-1000-000110 id 0000}
const GUID kClsidZip = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0x01, 0x00, 0x00}
};
const GUID kClsid7z = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0x07, 0x00, 0x00}
};
const GUID kClsidRar = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0x03, 0x00, 0x00}
};
const GUID kClsidRar5 = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0xCC, 0x00, 0x00}
};
const GUID kClsidTar = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0xEE, 0x00, 0x00}
};
const GUID kClsidGZip = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0xEF, 0x00, 0x00}
};
const GUID kClsidIso = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0xE7, 0x00, 0x00}
};
const GUID kClsidXz = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0x0C, 0x00, 0x00}
};

enum class ArchiveFormat {
    Zip,
    SevenZip,
    Rar,
    Tar,
    GZip,
    Iso,
    Xz
};


std::wstring LowerExtension(const std::wstring& path) {
    std::wstring extension = fs::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension;
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

ArchiveFormat DetectFormat(const std::wstring& path) {
    const std::wstring fileName = fs::path(path).filename().wstring();
    if (EndsWithNoCase(fileName, L".tar.gz") || EndsWithNoCase(fileName, L".tgz")) {
        return ArchiveFormat::GZip;
    }
    if (EndsWithNoCase(fileName, L".tar.xz") || EndsWithNoCase(fileName, L".txz")) {
        return ArchiveFormat::Xz;
    }

    const std::wstring extension = LowerExtension(path);
    if (extension == L".zip" || extension == L".zipx" || extension == L".z01") {
        return ArchiveFormat::Zip;
    }
    if (extension == L".rar") {
        return ArchiveFormat::Rar;
    }
    if (extension == L".tar") {
        return ArchiveFormat::Tar;
    }
    if (extension == L".iso" || extension == L".img") {
        return ArchiveFormat::Iso;
    }
    if (extension == L".gz") {
        return ArchiveFormat::GZip;
    }
    if (extension == L".xz") {
        return ArchiveFormat::Xz;
    }
    return ArchiveFormat::SevenZip;
}

std::vector<const GUID*> ClsidsForFormat(ArchiveFormat format) {
    switch (format) {
    case ArchiveFormat::Zip:
        return {&kClsidZip};
    case ArchiveFormat::Rar:
        return {&kClsidRar, &kClsidRar5};
    case ArchiveFormat::Tar:
        return {&kClsidTar};
    case ArchiveFormat::GZip:
        return {&kClsidGZip};
    case ArchiveFormat::Iso:
        return {&kClsidIso};
    case ArchiveFormat::Xz:
        return {&kClsidXz};
    case ArchiveFormat::SevenZip:
    default:
        return {&kClsid7z};
    }
}

bool IsSafeRelativePath(const std::wstring& entryName, fs::path& relative) {
    if (entryName.empty()) {
        return false;
    }

    std::wstring normalized = entryName;
    std::replace(normalized.begin(), normalized.end(), L'\\', L'/');

    if (normalized.find(L':') != std::wstring::npos) {
        return false;
    }

    const fs::path candidate(normalized);
    if (candidate.is_absolute() ||
        candidate.has_root_name() ||
        candidate.has_root_directory()) {
        return false;
    }

    for (const fs::path& part : candidate) {
        if (part == L"..") {
            return false;
        }
    }

    relative = candidate.lexically_normal();
    if (relative.empty()) {
        return false;
    }

    const auto first = relative.begin();
    if (first != relative.end() && *first == L"..") {
        return false;
    }

    return true;
}

class CInFileStream final : public IInStream, public CMyUnknownImp {
    Z7_COM_UNKNOWN_IMP_2(ISequentialInStream, IInStream)

public:
    CInFileStream() = default;
    ~CInFileStream() {
        Close();
    }

    bool Open(const std::wstring& path) {
        Close();
        _handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return _handle != INVALID_HANDLE_VALUE;
    }

    void Close() {
        if (_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(_handle);
            _handle = INVALID_HANDLE_VALUE;
        }
    }

    Z7_COM7F_IMF(Read(void* data, UInt32 size, UInt32* processedSize)) override {
        if (processedSize != nullptr) {
            *processedSize = 0;
        }
        if (_handle == INVALID_HANDLE_VALUE) {
            return S_FALSE;
        }

        DWORD read = 0;
        if (!ReadFile(_handle, data, size, &read, nullptr)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (processedSize != nullptr) {
            *processedSize = read;
        }
        return S_OK;
    }

    Z7_COM7F_IMF(Seek(Int64 offset, UInt32 seekOrigin, UInt64* newPosition)) override {
        if (_handle == INVALID_HANDLE_VALUE) {
            return S_FALSE;
        }

        DWORD moveMethod = FILE_BEGIN;
        if (seekOrigin == STREAM_SEEK_CUR) {
            moveMethod = FILE_CURRENT;
        } else if (seekOrigin == STREAM_SEEK_END) {
            moveMethod = FILE_END;
        }

        LARGE_INTEGER distance{};
        distance.QuadPart = offset;
        LARGE_INTEGER result{};
        if (!SetFilePointerEx(_handle, distance, &result, moveMethod)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (newPosition != nullptr) {
            *newPosition = static_cast<UInt64>(result.QuadPart);
        }
        return S_OK;
    }

private:
    HANDLE _handle = INVALID_HANDLE_VALUE;
};

class COutFileStream final : public ISequentialOutStream, public CMyUnknownImp {
    Z7_COM_UNKNOWN_IMP_1(ISequentialOutStream)

public:
    std::atomic<bool>* pauseRequested = nullptr;
    std::atomic<bool>* cancelRequested = nullptr;
    HWND hwnd = nullptr;
    UInt64 entrySize = 0;
    UInt64 entryWritten = 0;
    int progressBase = 0;
    int progressRange = 100;
    bool archiveProgressAvailable = false;

    ~COutFileStream() {
        Close();
    }

    bool Create(const std::wstring& path) {
        Close();
        _handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return _handle != INVALID_HANDLE_VALUE;
    }

    void Close() {
        if (_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(_handle);
            _handle = INVALID_HANDLE_VALUE;
        }
    }

    Z7_COM7F_IMF(Write(const void* data, UInt32 size, UInt32* processedSize)) override {
        if (processedSize != nullptr) {
            *processedSize = 0;
        }
        if (_handle == INVALID_HANDLE_VALUE) {
            return S_FALSE;
        }

        while (pauseRequested != nullptr && pauseRequested->load()) {
            if (cancelRequested != nullptr && cancelRequested->load()) {
                return E_ABORT;
            }
            Sleep(20);
        }
        if (cancelRequested != nullptr && cancelRequested->load()) {
            return E_ABORT;
        }

        DWORD written = 0;
        if (!WriteFile(_handle, data, size, &written, nullptr)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (processedSize != nullptr) {
            *processedSize = written;
        }

        entryWritten += written;
        if (hwnd != nullptr && !archiveProgressAvailable && entrySize > 0) {
            int percent = progressBase + static_cast<int>(
                (entryWritten * static_cast<UInt64>(progressRange)) / entrySize);
            if (percent < progressBase) percent = progressBase;
            if (percent > progressBase + progressRange) percent = progressBase + progressRange;
            PostMessageW(hwnd, WM_APP + 1, static_cast<WPARAM>(percent), 0);
        }
        return S_OK;
    }

private:
    HANDLE _handle = INVALID_HANDLE_VALUE;
};

void StripDuplicateTopDirectory(const fs::path& outputDir) {
    std::error_code ec;
    if (!fs::is_directory(outputDir, ec)) {
        return;
    }

    int count = 0;
    fs::path onlyDirectory;
    for (fs::directory_iterator it(outputDir, ec), end; !ec && it != end; it.increment(ec)) {
        ++count;
        std::error_code typeEc;
        if (it->is_directory(typeEc)) {
            onlyDirectory = it->path();
        } else {
            return;
        }
    }

    if (count != 1) {
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

    const fs::path temp = outputDir.parent_path() / (outputDir.filename().wstring() + L"_tmp_strip");
    fs::rename(onlyDirectory, temp, ec);
    if (ec) {
        return;
    }
    fs::remove(outputDir, ec);
    if (ec) {
        fs::rename(temp, onlyDirectory, ec);
        return;
    }
    fs::rename(temp, outputDir, ec);
}

class COpenCallback final : public IArchiveOpenCallback,
                            public IArchiveOpenVolumeCallback,
                            public ICryptoGetTextPassword,
                            public CMyUnknownImp {
    Z7_COM_QI_BEGIN2(IArchiveOpenCallback)
    Z7_COM_QI_ENTRY(IArchiveOpenVolumeCallback)
    Z7_COM_QI_ENTRY(ICryptoGetTextPassword)
    Z7_COM_QI_END
    Z7_COM_ADDREF_RELEASE
    Z7_IFACE_COM7_IMP(IArchiveOpenCallback)
    Z7_IFACE_COM7_IMP(IArchiveOpenVolumeCallback)
    Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)

public:
    std::wstring password;
    std::wstring baseDir;
    std::wstring archiveName;
};

Z7_COM7F_IMF(COpenCallback::SetTotal(const UInt64*, const UInt64*)) {
    return S_OK;
}

Z7_COM7F_IMF(COpenCallback::SetCompleted(const UInt64*, const UInt64*)) {
    return S_OK;
}

Z7_COM7F_IMF(COpenCallback::CryptoGetTextPassword(BSTR* passwordOut)) {

    *passwordOut = SysAllocString(password.c_str());
    return *passwordOut != nullptr ? S_OK : E_OUTOFMEMORY;
}

Z7_COM7F_IMF(COpenCallback::GetProperty(PROPID propID, PROPVARIANT* value)) {
    value->vt = VT_EMPTY;
    if (propID == kpidName) {
        value->vt = VT_BSTR;
        value->bstrVal = SysAllocString(archiveName.c_str());
        return value->bstrVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }
    return S_OK;
}

Z7_COM7F_IMF(COpenCallback::GetStream(const wchar_t* name, IInStream** inStream)) {
    *inStream = nullptr;
    if (name == nullptr) {
        return S_FALSE;
    }

    std::error_code ec;
    const fs::path path = fs::path(baseDir) / name;
    if (!fs::exists(path, ec)) {
        return S_FALSE;
    }

    CInFileStream* streamSpec = new CInFileStream;
    CMyComPtr<IInStream> stream(streamSpec);
    if (!streamSpec->Open(path.wstring())) {
        return S_FALSE;
    }

    *inStream = stream.Detach();
    return S_OK;
}

class CExtractCallback final : public IArchiveExtractCallback,
                               public ICryptoGetTextPassword,
                               public CMyUnknownImp {
    Z7_IFACES_IMP_UNK_2(IArchiveExtractCallback, ICryptoGetTextPassword)
    Z7_IFACE_COM7_IMP(IProgress)

public:
    CMyComPtr<IInArchive> archive;
    std::wstring outputDir;
    std::wstring password;
    HWND hwnd = nullptr;
    std::atomic<bool>* cancelRequested = nullptr;
    std::atomic<bool>* pauseRequested = nullptr;
    std::wstring fallbackEntryName;
    ExtractNotify* notify = nullptr;
    ConflictPolicy conflictPolicy = ConflictPolicy::Overwrite;

    UInt64 totalBytes = 0;
    int extracted = 0;
    int failed = 0;
    int skipped = 0;
    int renamed = 0;
    int progressBase = 0;
    int progressRange = 100;
    bool limitExceeded = false;
    unsigned long long bombBytes = 0;
    int bombEntries = 0;

    static constexpr unsigned long long kMaxExtractBytes =
        64ull * 1024 * 1024 * 1024;
    static constexpr int kMaxExtractEntries = 200000;

    bool isFile = false;
    COutFileStream* outFileSpec = nullptr;
    CMyComPtr<ISequentialOutStream> outStream;
    std::wstring currentOutputPath;
};

Z7_COM7F_IMF(CExtractCallback::SetTotal(UInt64 size)) {
    totalBytes = size;
    return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::SetCompleted(const UInt64* completeValue)) {
    while (pauseRequested != nullptr && pauseRequested->load()) {
        if (cancelRequested != nullptr && cancelRequested->load()) {
            return E_ABORT;
        }
        Sleep(20);
    }
    if (cancelRequested != nullptr && cancelRequested->load()) {
        return E_ABORT;
    }

    if (hwnd != nullptr && totalBytes > 0 && completeValue != nullptr) {
        int percent = progressBase + static_cast<int>(
            (*completeValue * static_cast<UInt64>(progressRange)) / totalBytes);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        PostMessageW(hwnd, WM_APP + 1, static_cast<WPARAM>(percent), 0);
    }
    return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::GetStream(UInt32 index,
                                         ISequentialOutStream** outStreamResult,
                                         Int32 askExtractMode)) {
    *outStreamResult = nullptr;
    outStream.Release();
    outFileSpec = nullptr;
    isFile = false;

    PROPVARIANT pathProp;
    PropVariantInit(&pathProp);
    HRESULT result = archive->GetProperty(index, kpidPath, &pathProp);
    if (result != S_OK) {
        PropVariantClear(&pathProp);
        return result;
    }

    std::wstring entryName;
    if (pathProp.vt == VT_BSTR && pathProp.bstrVal != nullptr) {
        entryName = pathProp.bstrVal;
    }
    PropVariantClear(&pathProp);


    if (entryName.empty() && !fallbackEntryName.empty()) {
        entryName = fallbackEntryName;
    }

    if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) {
        return S_OK;
    }

    fs::path relative;
    if (!IsSafeRelativePath(entryName, relative)) {
        ++skipped;
        return S_OK;
    }

    fs::path target = fs::path(outputDir) / relative;

    bool isDirectory = false;
    PROPVARIANT dirProp;
    PropVariantInit(&dirProp);
    if (archive->GetProperty(index, kpidIsDir, &dirProp) == S_OK &&
        dirProp.vt == VT_BOOL) {
        isDirectory = (dirProp.boolVal != VARIANT_FALSE);
    }
    PropVariantClear(&dirProp);

    std::error_code ec;
    if (isDirectory) {
        fs::create_directories(target, ec);
        if (ec) {
            ++failed;
        }
        return S_OK;
    }

    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        ++failed;
        return E_ABORT;
    }

    ec.clear();
    if (fs::exists(target, ec)) {
        if (conflictPolicy == ConflictPolicy::Skip) {
            ++skipped;
            return S_OK;
        }
        if (conflictPolicy == ConflictPolicy::Rename) {
            const std::wstring stem = target.stem().wstring();
            const std::wstring extension = target.extension().wstring();
            fs::path candidate;
            for (int suffix = 1; suffix < 10000; ++suffix) {
                candidate = target.parent_path() /
                    (stem + L"(" + std::to_wstring(suffix) + L")" + extension);
                std::error_code existsEc;
                if (!fs::exists(candidate, existsEc)) {
                    break;
                }
            }
            if (!candidate.empty()) {
                target = candidate;
                ++renamed;
            }
        }
    }

    if (notify != nullptr && !entryName.empty()) {
        (*notify)(entryName);
    }

    UInt64 entrySize = 0;
    PROPVARIANT sizeProp;
    PropVariantInit(&sizeProp);
    if (archive->GetProperty(index, kpidSize, &sizeProp) == S_OK) {
        if (sizeProp.vt == VT_UI8) {
            entrySize = sizeProp.uhVal.QuadPart;
        } else if (sizeProp.vt == VT_UI4) {
            entrySize = sizeProp.ulVal;
        }
    }
    PropVariantClear(&sizeProp);

    if (entrySize == 0) {
        entrySize = 1;
    }
    if (bombBytes + entrySize > kMaxExtractBytes ||
        bombEntries >= kMaxExtractEntries) {
        limitExceeded = true;
        return E_ABORT;
    }
    bombBytes += entrySize;
    ++bombEntries;

    outFileSpec = new COutFileStream;
    outFileSpec->pauseRequested = pauseRequested;
    outFileSpec->cancelRequested = cancelRequested;
    outFileSpec->hwnd = hwnd;
    outFileSpec->entrySize = entrySize;
    outFileSpec->progressBase = progressBase;
    outFileSpec->progressRange = progressRange;
    outFileSpec->archiveProgressAvailable = (totalBytes > 0);
    CMyComPtr<ISequentialOutStream> stream(outFileSpec);
    currentOutputPath = target.wstring();
    if (!outFileSpec->Create(target.wstring())) {
        outFileSpec = nullptr;
        ++failed;
        return E_ABORT;
    }

    isFile = true;
    *outStreamResult = stream.Detach();
    return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::PrepareOperation(Int32)) {
    return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::SetOperationResult(Int32 operationResult)) {
    if (isFile) {
        if (outFileSpec != nullptr) {
            outFileSpec->Close();
        }
        const bool wroteNothing =
            outFileSpec != nullptr && outFileSpec->entrySize > 0 &&
            outFileSpec->entryWritten == 0;
        if (operationResult == NArchive::NExtract::NOperationResult::kOK &&
            !wroteNothing) {
            ++extracted;
        } else {
            ++failed;
            if (!currentOutputPath.empty()) {
                DeleteFileW(currentOutputPath.c_str());
            }
        }
    }

    outStream.Release();
    outFileSpec = nullptr;
    currentOutputPath.clear();
    isFile = false;
    return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::CryptoGetTextPassword(BSTR* passwordOut)) {
    *passwordOut = SysAllocString(password.c_str());
    return *passwordOut != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT OpenEngineArchive(const std::wstring& archivePath,
                          ArchiveFormat format,
                          const std::wstring& password,
                          CMyComPtr<IInArchive>& archive,
                          CMyComPtr<IInStream>& stream) {
    HRESULT lastResult = E_FAIL;

    for (const GUID* clsid : ClsidsForFormat(format)) {
        void* object = nullptr;
        HRESULT result = CreateObject(clsid, &IID_IInArchive, &object);
        if (result != S_OK || object == nullptr) {
            lastResult = (result != S_OK) ? result : E_FAIL;
            continue;
        }

        CMyComPtr<IInArchive> candidate;
        candidate.Attach(reinterpret_cast<IInArchive*>(object));

        CInFileStream* streamSpec = new CInFileStream;
        CMyComPtr<IInStream> candidateStream(streamSpec);
        if (!streamSpec->Open(archivePath)) {
            candidate->Close();
            lastResult = E_FAIL;
            continue;
        }

        COpenCallback* openSpec = new COpenCallback;
        CMyComPtr<IArchiveOpenCallback> openCallback(openSpec);
        openSpec->password = password;
        openSpec->baseDir = fs::path(archivePath).parent_path().wstring();
        openSpec->archiveName = fs::path(archivePath).filename().wstring();

        const HRESULT openResult = candidate->Open(candidateStream, nullptr, openCallback);
        if (openResult == S_OK) {
            archive = candidate;
            stream = candidateStream;
            return S_OK;
        }

        candidate->Close();
        lastResult = openResult;
    }

    return lastResult;
}

std::mutex g_archiveMutex;

struct CachedArchiveState {
    std::wstring path;
    std::wstring password;
    ArchiveFormat format = ArchiveFormat::Zip;
    CMyComPtr<IInArchive> archive;
    CMyComPtr<IInStream> stream;
};

CachedArchiveState g_cachedArchive;

HRESULT OpenCachedArchiveLocked(const std::wstring& path, ArchiveFormat format,
                                const std::wstring& password,
                                CMyComPtr<IInArchive>& archive,
                                CMyComPtr<IInStream>& stream) {
    if (g_cachedArchive.archive != nullptr && g_cachedArchive.path == path &&
        g_cachedArchive.password == password &&
        g_cachedArchive.format == format) {
        archive = g_cachedArchive.archive;
        stream = g_cachedArchive.stream;
        return S_OK;
    }
    if (g_cachedArchive.archive != nullptr) {
        g_cachedArchive.archive->Close();
        g_cachedArchive.archive.Release();
        g_cachedArchive.stream.Release();
    }
    CMyComPtr<IInArchive> opened;
    CMyComPtr<IInStream> openedStream;
    const HRESULT result =
        OpenEngineArchive(path, format, password, opened, openedStream);
    if (result == S_OK) {
        g_cachedArchive.path = path;
        g_cachedArchive.password = password;
        g_cachedArchive.format = format;
        g_cachedArchive.archive = opened;
        g_cachedArchive.stream = openedStream;
        archive = opened;
        stream = openedStream;
    }
    return result;
}

bool ExtractDirectArchive(const std::wstring& archivePath,
                           ArchiveFormat format,
                           const fs::path& outputDir,
                           const std::wstring& password,
                           HWND hwnd,
                           std::atomic<bool>& cancelRequested,
                           std::atomic<bool>& pauseRequested,
                           int progressBase,
                           int progressRange,
                           int& extracted,
                           int& failed,
                           std::wstring& message,
                           bool stripTopDirectory,
                           const std::wstring& fallbackEntryName = std::wstring(),
                           ExtractNotify* notify = nullptr,
                           ConflictPolicy conflictPolicy =
                               ConflictPolicy::Overwrite) {
    extracted = 0;
    failed = 0;
    message.clear();

    CMyComPtr<IInArchive> archive;
    CMyComPtr<IInStream> stream;

    const HRESULT openResult = OpenEngineArchive(
        archivePath, format, password, archive, stream);
    if (openResult == S_FALSE) {
        message = L"需要密码";
        return false;
    }
    if (openResult != S_OK || !archive) {
        message = L"无法打开压缩包：格式不支持或文件损坏。";
        return false;
    }

    std::error_code ec;
    fs::create_directories(outputDir, ec);

    CExtractCallback* extractSpec = new CExtractCallback;
    CMyComPtr<IArchiveExtractCallback> extractCallback(extractSpec);
    extractSpec->archive = archive;
    extractSpec->outputDir = outputDir.wstring();
    extractSpec->password = password;
    extractSpec->hwnd = hwnd;
    extractSpec->cancelRequested = &cancelRequested;
    extractSpec->pauseRequested = &pauseRequested;
    extractSpec->fallbackEntryName = fallbackEntryName;
    extractSpec->notify = notify;
    extractSpec->conflictPolicy = conflictPolicy;
    extractSpec->progressBase = progressBase;
    extractSpec->progressRange = progressRange;

    const UInt32 allItems = static_cast<UInt32>(static_cast<Int32>(-1));
    const HRESULT extractResult = archive->Extract(
        nullptr, allItems, NArchive::NExtract::NAskMode::kExtract, extractCallback);

    archive->Close();

    extracted = extractSpec->extracted;
    failed = extractSpec->failed;
    const int skipped = extractSpec->skipped;
    const int renamed = extractSpec->renamed;

    if (cancelRequested.load()) {
        message = L"已取消解压。";
        return false;
    }

    if (extractResult != S_OK && extracted == 0) {
        message = L"解压失败：密码错误、文件损坏或格式不支持。";
        return false;
    }

    if (extracted == 0 && failed > 0) {
        message = L"解压失败：密码错误、文件损坏或格式不支持。";
        return false;
    }

    if (extracted == 0 && failed == 0 && skipped == 0) {
        message = L"压缩包是空的，没有可解压的文件。";
        return false;
    }

    if (stripTopDirectory && extracted > 0) {
        StripDuplicateTopDirectory(outputDir);
    }

    if (skipped > 0) {
        message = L"解压完成：成功 " + std::to_wstring(extracted) +
                  L" 个文件，安全跳过 " + std::to_wstring(skipped) +
                  L" 个，失败 " + std::to_wstring(failed) + L" 个。";
    } else {
        message = L"解压完成：成功 " + std::to_wstring(extracted) +
                  L" 个文件，失败 " + std::to_wstring(failed) + L" 个。";
    }
    if (renamed > 0) {
        message += L"（保留两者：重命名 " + std::to_wstring(renamed) +
                   L" 个冲突文件）";
    }
    if (extractSpec->limitExceeded) {
        message = L"已中止：解压内容超过安全阈值，可能为解压炸弹。";
        return false;
    }
    return true;
}

fs::path FindFirstRegularFile(const fs::path& dir) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc)) {
            return it->path();
        }
    }
    return fs::path();
}

bool IsTarFile(const fs::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    char header[512] = {};
    DWORD bytesRead = 0;
    const bool readOk = ReadFile(handle, header, sizeof(header), &bytesRead, nullptr) &&
                        bytesRead == sizeof(header);
    CloseHandle(handle);
    return readOk && std::memcmp(header + 257, "ustar", 5) == 0;
}

std::wstring StripCompressionSuffix(const std::wstring& fileName) {
    std::wstring name = fileName;
    for (const wchar_t* suffix : {L".gz", L".xz"}) {
        const std::wstring suffixText(suffix);
        const size_t length = suffixText.size();
        if (name.size() > length && EndsWithNoCase(name, suffix)) {
            name.resize(name.size() - length);
            break;
        }
    }
    return name.empty() ? std::wstring(L"output") : name;
}

fs::path CreateTgzTempDir() {
    std::error_code ec;
    const fs::path tempBase = fs::temp_directory_path(ec);
    if (ec) {
        return fs::path();
    }

    const fs::path tempDir = tempBase /
        (L"PopZip_tgz_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()));
    fs::create_directories(tempDir, ec);
    if (ec) {
        return fs::path();
    }
    return tempDir;
}

} // namespace

namespace {

bool IsSplitFirstVolume(const std::wstring& path) {
    return fs::path(path).extension().wstring() == L".001";
}

bool JoinSplitVolumes(const std::wstring& firstPath, std::wstring& mergedPath) {
    const fs::path first(firstPath);
    const fs::path base = first.parent_path() / first.stem();
    std::error_code ec;
    const fs::path tempDir = fs::temp_directory_path(ec);
    if (ec) {
        return false;
    }
    std::wstring baseExt = fs::path(first.stem()).extension().wstring();
    if (baseExt.empty()) {
        baseExt = L".7z";
    }
    const fs::path temp = tempDir /
        (L"PopZip_join_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()) + baseExt);

    HANDLE out = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool ok = true;
    bool any = false;
    char buffer[1 << 16];
    for (int i = 1; i <= 999; ++i) {
        wchar_t suffix[8] = {};
        swprintf(suffix, 8, L".%03d", i);
        const fs::path part = base.wstring() + std::wstring(suffix);
        std::error_code existsEc;
        if (!fs::exists(part, existsEc)) {
            if (i == 1) {
                ok = false;
            }
            break;
        }
        any = true;

        HANDLE in = CreateFileW(part.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
        if (in == INVALID_HANDLE_VALUE) {
            ok = false;
            break;
        }
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(in, buffer, sizeof(buffer), &read, nullptr) ||
                read == 0) {
                break;
            }
            DWORD written = 0;
            if (!WriteFile(out, buffer, read, &written, nullptr) ||
                written != read) {
                ok = false;
                break;
            }
        }
        CloseHandle(in);
        if (!ok) {
            break;
        }
    }

    CloseHandle(out);
    if (!ok || !any) {
        DeleteFileW(temp.c_str());
        return false;
    }
    mergedPath = temp.wstring();
    return true;
}

struct TempFileCleaner {
    std::wstring path;
    ~TempFileCleaner() {
        if (!path.empty()) {
            DeleteFileW(path.c_str());
        }
    }
};

} // namespace

bool SevenZipNeedsPassword(const std::wstring& archivePath) {
    const ArchiveFormat format = DetectFormat(archivePath);
    if (format == ArchiveFormat::Tar || format == ArchiveFormat::GZip ||
        format == ArchiveFormat::Iso || format == ArchiveFormat::Xz) {
        return false;
    }

    CMyComPtr<IInArchive> archive;
    CMyComPtr<IInStream> stream;

    const HRESULT openResult = OpenEngineArchive(archivePath, format, L"", archive, stream);

    if (openResult != S_OK) {
        return true;
    }

    UInt32 count = 0;
    if (archive->GetNumberOfItems(&count) != S_OK) {
        archive->Close();
        return true;
    }


    bool needsPassword = false;
    for (UInt32 i = 0; i < count; ++i) {
        PROPVARIANT prop;
        PropVariantInit(&prop);
        if (archive->GetProperty(i, kpidEncrypted, &prop) == S_OK &&
            prop.vt == VT_BOOL &&
            prop.boolVal != VARIANT_FALSE) {
            needsPassword = true;
        }
        PropVariantClear(&prop);
        if (needsPassword) {
            break;
        }
    }

    archive->Close();
    return needsPassword;
}

class CSingleFileCallback final : public IArchiveExtractCallback,
                                  public CMyUnknownImp {
    Z7_IFACES_IMP_UNK_1(IArchiveExtractCallback)
    Z7_IFACE_COM7_IMP(IProgress)

public:
    CMyComPtr<IInArchive> archive;
    std::wstring outputFile;
    CMyComPtr<ISequentialOutStream> outStream;
    bool failed = false;
    bool written = false;
};

Z7_COM7F_IMF(CSingleFileCallback::SetTotal(UInt64)) {
    return S_OK;
}

Z7_COM7F_IMF(CSingleFileCallback::SetCompleted(const UInt64*)) {
    return S_OK;
}

Z7_COM7F_IMF(CSingleFileCallback::GetStream(UInt32 index,
                                            ISequentialOutStream** outStream,
                                            Int32)) {
    if (outStream != nullptr) {
        *outStream = nullptr;
    }
    PROPVARIANT prop;
    PropVariantInit(&prop);
    bool isDir = false;
    if (archive != nullptr &&
        archive->GetProperty(index, kpidIsDir, &prop) == S_OK &&
        prop.vt == VT_BOOL) {
        isDir = prop.boolVal != VARIANT_FALSE;
    }
    PropVariantClear(&prop);
    if (isDir) {
        return S_OK;
    }

    COutFileStream* spec = new COutFileStream;
    CMyComPtr<ISequentialOutStream> stream(spec);
    if (!spec->Create(outputFile)) {
        failed = true;
        return E_ABORT;
    }
    written = true;
    if (outStream != nullptr) {
        *outStream = stream.Detach();
    }
    return S_OK;
}

Z7_COM7F_IMF(CSingleFileCallback::PrepareOperation(Int32)) {
    return S_OK;
}

Z7_COM7F_IMF(CSingleFileCallback::SetOperationResult(Int32 operationResult)) {
    if (operationResult != NArchive::NExtract::NOperationResult::kOK) {
        failed = true;
    }
    outStream.Release();
    return S_OK;
}

class CTestCallback final : public IArchiveExtractCallback,
                            public CMyUnknownImp {
    Z7_IFACES_IMP_UNK_1(IArchiveExtractCallback)
    Z7_IFACE_COM7_IMP(IProgress)

public:
    CMyComPtr<IInArchive> archive;
    std::vector<std::wstring>* failed = nullptr;
    std::wstring currentPath;
    int failCount = 0;
};

Z7_COM7F_IMF(CTestCallback::SetTotal(UInt64)) {
    return S_OK;
}

Z7_COM7F_IMF(CTestCallback::SetCompleted(const UInt64*)) {
    return S_OK;
}

Z7_COM7F_IMF(CTestCallback::GetStream(UInt32 index,
                                      ISequentialOutStream** outStream,
                                      Int32)) {
    if (outStream != nullptr) {
        *outStream = nullptr;
    }
    currentPath.clear();
    PROPVARIANT prop;
    PropVariantInit(&prop);
    if (archive != nullptr &&
        archive->GetProperty(index, kpidPath, &prop) == S_OK &&
        prop.vt == VT_BSTR && prop.bstrVal != nullptr) {
        currentPath = prop.bstrVal;
    }
    PropVariantClear(&prop);
    return S_OK;
}

Z7_COM7F_IMF(CTestCallback::PrepareOperation(Int32)) {
    return S_OK;
}

Z7_COM7F_IMF(CTestCallback::SetOperationResult(Int32 operationResult)) {
    if (operationResult != NArchive::NExtract::NOperationResult::kOK) {
        ++failCount;
        if (failed != nullptr) {
            failed->push_back(currentPath);
        }
    }
    return S_OK;
}

bool ExtractArchiveEntry(const std::wstring& archivePath,
                         const std::wstring& password,
                         int index,
                         const std::wstring& outputFile,
                         std::wstring& message,
                         const std::wstring& actualPathOverride) {
    message.clear();
    if (index < 0) {
        message = L"无效的条目。";
        return false;
    }

    std::wstring merged;
    std::wstring actualPath = actualPathOverride;
    if (actualPath.empty()) {
        actualPath = archivePath;
        if (IsSplitFirstVolume(archivePath)) {
            if (!JoinSplitVolumes(archivePath, merged)) {
                message = L"无法合并分卷文件。";
                return false;
            }
            actualPath = merged;
        }
    }
    TempFileCleaner cleaner{merged};

    const ArchiveFormat format = DetectFormat(actualPath);
    const bool useCache = !actualPathOverride.empty();
    std::unique_lock<std::mutex> cacheLock(g_archiveMutex, std::defer_lock);
    if (useCache) {
        cacheLock.lock();
    }
    CMyComPtr<IInArchive> archive;
    CMyComPtr<IInStream> stream;
    const HRESULT openResult =
        useCache ? OpenCachedArchiveLocked(actualPath, format, password, archive,
                                           stream)
                 : OpenEngineArchive(actualPath, format, password, archive, stream);
    if (openResult != S_OK || !archive) {
        message = L"无法打开压缩包。";
        return false;
    }

    CSingleFileCallback* spec = new CSingleFileCallback;
    spec->archive = archive;
    spec->outputFile = outputFile;
    CMyComPtr<IArchiveExtractCallback> callback(spec);

    const UInt32 itemIndex = static_cast<UInt32>(index);
    const HRESULT extractResult =
        archive->Extract(&itemIndex, 1, NArchive::NExtract::NAskMode::kExtract,
                         callback);
    if (!useCache) {
        archive->Close();
    }

    if (extractResult != S_OK || spec->failed || !spec->written) {
        message = L"无法提取该条目。";
        return false;
    }
    return true;
}

bool TestArchive(const std::wstring& archivePath,
                 const std::wstring& password,
                 std::vector<std::wstring>& failedEntries,
                 std::wstring& message,
                 const std::wstring& actualPathOverride) {
    failedEntries.clear();
    message.clear();

    std::wstring merged;
    std::wstring path = actualPathOverride;
    if (path.empty()) {
        path = archivePath;
        const std::wstring fileName = fs::path(path).filename().wstring();
        if (EndsWithNoCase(fileName, L".z01")) {
            const fs::path p(path);
            path = (p.parent_path() / (p.stem().wstring() + L".zip")).wstring();
        }
        if (IsSplitFirstVolume(path)) {
            if (!JoinSplitVolumes(path, merged)) {
                message = L"无法合并分卷文件。";
                return false;
            }
            path = merged;
        }
    }
    TempFileCleaner cleaner{merged};

    const ArchiveFormat format = DetectFormat(path);
    const bool useCache = !actualPathOverride.empty();
    std::unique_lock<std::mutex> cacheLock(g_archiveMutex, std::defer_lock);
    if (useCache) {
        cacheLock.lock();
    }

    CMyComPtr<IInArchive> archive;
    CMyComPtr<IInStream> stream;
    const HRESULT openResult =
        useCache ? OpenCachedArchiveLocked(path, format, password, archive,
                                           stream)
                 : OpenEngineArchive(path, format, password, archive, stream);
    if (openResult == S_FALSE) {
        message = L"需要密码或分卷不完整。";
        return false;
    }
    if (openResult != S_OK || !archive) {
        message = L"无法打开压缩包：格式不支持或文件损坏。";
        return false;
    }

    UInt32 count = 0;
    archive->GetNumberOfItems(&count);

    CTestCallback* spec = new CTestCallback;
    CMyComPtr<IArchiveExtractCallback> callback(spec);
    spec->archive = archive;
    spec->failed = &failedEntries;

    const UInt32 allItems = static_cast<UInt32>(static_cast<Int32>(-1));
    const HRESULT result = archive->Extract(
        nullptr, allItems, NArchive::NExtract::NAskMode::kTest, callback);
    if (!useCache) {
        archive->Close();
    }

    const int failedCount = static_cast<int>(failedEntries.size());
    if (result != S_OK && failedCount == 0) {
        message = L"测试失败：无法读取压缩包内容。";
        return false;
    }
    if (failedCount == 0) {
        message = L"测试完成：共 " + std::to_wstring(count) + L" 项，全部正常。";
        return true;
    }
    message = L"测试完成：共 " + std::to_wstring(count) + L" 项，损坏 " +
              std::to_wstring(failedCount) + L" 项。";
    return false;
}

namespace {

class CAppendOutStream final : public IOutStream, public CMyUnknownImp {
    Z7_COM_UNKNOWN_IMP_2(ISequentialOutStream, IOutStream)

public:
    ~CAppendOutStream() { Close(); }

    bool Create(const std::wstring& path) {
        Close();
        _handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        _position = 0;
        return _handle != INVALID_HANDLE_VALUE;
    }

    void Close() {
        if (_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(_handle);
            _handle = INVALID_HANDLE_VALUE;
        }
    }

    Z7_COM7F_IMF(Write(const void* data, UInt32 size, UInt32* processedSize)) override {
        if (processedSize != nullptr) {
            *processedSize = 0;
        }
        DWORD written = 0;
        if (!WriteFile(_handle, data, size, &written, nullptr)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        _position += written;
        if (processedSize != nullptr) {
            *processedSize = written;
        }
        return S_OK;
    }

    Z7_COM7F_IMF(Seek(Int64 offset, UInt32 seekOrigin, UInt64* newPosition)) override {
        LARGE_INTEGER distance{};
        distance.QuadPart = offset;
        LARGE_INTEGER result{};
        if (!SetFilePointerEx(_handle, distance, &result, seekOrigin)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        _position = static_cast<UInt64>(result.QuadPart);
        if (newPosition != nullptr) {
            *newPosition = _position;
        }
        return S_OK;
    }

    Z7_COM7F_IMF(SetSize(UInt64 newSize)) override {
        LARGE_INTEGER target{};
        target.QuadPart = static_cast<LONGLONG>(newSize);
        if (!SetFilePointerEx(_handle, target, nullptr, FILE_BEGIN)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (!SetEndOfFile(_handle)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }

private:
    HANDLE _handle = INVALID_HANDLE_VALUE;
    UInt64 _position = 0;
};

std::wstring NormalizePath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'\\', L'/');
    return path;
}

class CAppendCallback final : public IArchiveUpdateCallback,
                              public ICryptoGetTextPassword2,
                              public CMyUnknownImp {
    Z7_IFACES_IMP_UNK_2(IArchiveUpdateCallback, ICryptoGetTextPassword2)
    Z7_IFACE_COM7_IMP(IProgress)

public:
    CMyComPtr<IInArchive> archive;
    std::vector<std::wstring> files;
    UInt32 oldCount = 0;
    std::wstring password;
    std::wstring targetPrefix;
    bool overwrite = false;
    std::set<std::wstring> newPaths;
};

Z7_COM7F_IMF(CAppendCallback::SetTotal(UInt64)) {
    return S_OK;
}

Z7_COM7F_IMF(CAppendCallback::SetCompleted(const UInt64*)) {
    return S_OK;
}

Z7_COM7F_IMF(CAppendCallback::GetUpdateItemInfo(UInt32 index, Int32* newData,
                                                Int32* newProperties,
                                                UInt32* indexInArchive)) {
    if (index < oldCount) {
        bool conflict = false;
        if (overwrite && archive != nullptr) {
            PROPVARIANT pathProp;
            PropVariantInit(&pathProp);
            if (archive->GetProperty(index, kpidPath, &pathProp) == S_OK &&
                pathProp.vt == VT_BSTR && pathProp.bstrVal != nullptr &&
                newPaths.count(NormalizePath(pathProp.bstrVal)) > 0) {
                conflict = true;
            }
            PropVariantClear(&pathProp);
        }
        if (newData != nullptr) *newData = 0;
        if (newProperties != nullptr) *newProperties = conflict ? 1 : 0;
        if (indexInArchive != nullptr) *indexInArchive = index;
    } else {
        if (newData != nullptr) *newData = 1;
        if (newProperties != nullptr) *newProperties = 1;
        if (indexInArchive != nullptr) {
            *indexInArchive = static_cast<UInt32>(static_cast<Int32>(-1));
        }
    }
    return S_OK;
}

Z7_COM7F_IMF(CAppendCallback::GetProperty(UInt32 index, PROPID propID,
                                          PROPVARIANT* value)) {
    PropVariantInit(value);
    if (index < oldCount) {
        if (propID == kpidIsAnti) {
            value->vt = VT_BOOL;
            value->boolVal = VARIANT_FALSE;
            if (overwrite && archive != nullptr) {
                PROPVARIANT pathProp;
                PropVariantInit(&pathProp);
                if (archive->GetProperty(index, kpidPath, &pathProp) == S_OK &&
                    pathProp.vt == VT_BSTR && pathProp.bstrVal != nullptr &&
                    newPaths.count(NormalizePath(pathProp.bstrVal)) > 0) {
                    value->boolVal = VARIANT_TRUE;
                }
                PropVariantClear(&pathProp);
            }
            return S_OK;
        }
        if (archive != nullptr) {
            return archive->GetProperty(index, propID, value);
        }
        return S_OK;
    }

    const UInt32 newIndex = index - oldCount;
    if (newIndex >= files.size()) {
        return E_INVALIDARG;
    }
    const std::wstring& file = files[newIndex];

    switch (propID) {
    case kpidPath: {
        value->vt = VT_BSTR;
        std::wstring name = targetPrefix;
        if (!name.empty() && name.back() != L'\\') {
            name += L'\\';
        }
        name += fs::path(file).filename().wstring();
        value->bstrVal = SysAllocString(name.c_str());
        if (value->bstrVal == nullptr) {
            return E_OUTOFMEMORY;
        }
        break;
    }
    case kpidIsDir:
        value->vt = VT_BOOL;
        value->boolVal = VARIANT_FALSE;
        break;
    case kpidSize: {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        UInt64 size = 0;
        if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) {
            size = (static_cast<UInt64>(data.nFileSizeHigh) << 32) |
                   data.nFileSizeLow;
        }
        value->vt = VT_UI8;
        value->uhVal.QuadPart = size;
        break;
    }
    case kpidAttrib:
        value->vt = VT_UI4;
        value->ulVal = GetFileAttributesW(file.c_str());
        break;
    case kpidMTime: {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) {
            value->vt = VT_FILETIME;
            value->filetime = data.ftLastWriteTime;
        }
        break;
    }
    case kpidIsAnti:
        value->vt = VT_BOOL;
        value->boolVal = VARIANT_FALSE;
        break;
    default:
        break;
    }
    return S_OK;
}

Z7_COM7F_IMF(CAppendCallback::GetStream(UInt32 index,
                                        ISequentialInStream** inStream)) {
    if (inStream != nullptr) {
        *inStream = nullptr;
    }
    if (index < oldCount) {
        return S_OK;
    }
    const UInt32 newIndex = index - oldCount;
    if (newIndex >= files.size() || inStream == nullptr) {
        return E_INVALIDARG;
    }

    CInFileStream* spec = new CInFileStream;
    CMyComPtr<ISequentialInStream> stream(spec);
    if (!spec->Open(files[newIndex])) {
        return E_FAIL;
    }
    *inStream = stream.Detach();
    return S_OK;
}

Z7_COM7F_IMF(CAppendCallback::SetOperationResult(Int32)) {
    return S_OK;
}

Z7_COM7F_IMF(CAppendCallback::CryptoGetTextPassword2(Int32* passwordIsDefined,
                                                     BSTR* passwordOut)) {
    if (passwordIsDefined != nullptr) {
        *passwordIsDefined = 0;
    }
    if (passwordOut != nullptr) {
        *passwordOut = nullptr;
    }
    if (password.empty()) {
        return S_OK;
    }
    if (passwordIsDefined != nullptr) {
        *passwordIsDefined = 1;
    }
    if (passwordOut != nullptr) {
        *passwordOut = SysAllocString(password.c_str());
        if (*passwordOut == nullptr) {
            return E_OUTOFMEMORY;
        }
    }
    return S_OK;
}

} // namespace

namespace {

bool RebuildZipWithOverwrite(const std::wstring& archivePath,
                             const std::wstring& password,
                             const std::vector<std::wstring>& files,
                             const std::wstring& targetPrefix,
                             std::wstring& message) {
    std::error_code ec;
    const fs::path tempBase = fs::temp_directory_path(ec);
    if (ec) {
        message = L"无法创建临时目录。";
        return false;
    }
    const fs::path tempDir = tempBase /
        (L"PopZip_rebuild_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()));
    fs::create_directories(tempDir, ec);
    if (ec) {
        message = L"无法创建临时目录。";
        return false;
    }

    std::atomic<bool> cancel{false};
    std::atomic<bool> pause{false};
    int extracted = 0;
    int failed = 0;
    std::wstring extractMessage;
    if (!ExtractWithSevenZip(archivePath, tempDir, password, nullptr, cancel,
                             pause, extracted, failed, extractMessage)) {
        fs::remove_all(tempDir, ec);
        message = extractMessage.empty() ? L"无法解包原压缩包。"
                                          : extractMessage;
        return false;
    }

    for (const std::wstring& file : files) {
        std::wstring relative = targetPrefix;
        if (!relative.empty() && relative.back() != L'\\') {
            relative += L'\\';
        }
        relative += fs::path(file).filename().wstring();
        const fs::path target = tempDir / fs::path(relative);
        std::error_code removeEc;
        fs::remove(target, removeEc);
        std::error_code dirEc;
        fs::create_directories(target.parent_path(), dirEc);
        std::error_code copyEc;
        fs::copy_file(file, target, fs::copy_options::overwrite_existing,
                      copyEc);
        if (copyEc) {
            fs::remove_all(tempDir, ec);
            message = L"无法写入新文件。";
            return false;
        }
    }

    CompressOptions options;
    for (fs::directory_iterator it(tempDir, ec), end; !ec && it != end;
         it.increment(ec)) {
        options.sources.push_back(it->path());
    }
    options.outputFile = archivePath + L".rebuild";
    options.format = CompressFormat::Zip;
    options.password = password;
    int added = 0;
    std::wstring compressMessage;
    if (!CompressItems(options, nullptr, cancel, pause, added,
                       compressMessage)) {
        fs::remove_all(tempDir, ec);
        DeleteFileW((archivePath + L".rebuild").c_str());
        message = compressMessage;
        return false;
    }

    DeleteFileW(archivePath.c_str());
    if (!MoveFileExW((archivePath + L".rebuild").c_str(), archivePath.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        fs::remove_all(tempDir, ec);
        message = L"无法替换原压缩包。";
        return false;
    }

    fs::remove_all(tempDir, ec);
    message = L"已覆盖 " + std::to_wstring(files.size()) + L" 个文件。";
    return true;
}

} // namespace

bool RebuildArchiveEntries(const std::wstring& archivePath,
                           const std::wstring& password,
                           const std::vector<std::wstring>& removePaths,
                           const std::wstring& renameFrom,
                           const std::wstring& renameTo,
                           std::wstring& message) {
    const ArchiveFormat archiveFormat = DetectFormat(archivePath);
    if (archiveFormat != ArchiveFormat::Zip &&
        archiveFormat != ArchiveFormat::SevenZip) {
        message = L"仅支持修改 ZIP / 7Z 压缩包。";
        return false;
    }

    std::error_code ec;
    const fs::path tempBase = fs::temp_directory_path(ec);
    if (ec) {
        message = L"无法创建临时目录。";
        return false;
    }
    const fs::path tempDir =
        tempBase / (L"PopZip_rebuild_" + std::to_wstring(GetCurrentProcessId()) +
                    L"_" + std::to_wstring(GetTickCount64()));
    fs::create_directories(tempDir, ec);
    if (ec) {
        message = L"无法创建临时目录。";
        return false;
    }

    std::atomic<bool> cancel{false};
    std::atomic<bool> pause{false};
    int extracted = 0;
    int failed = 0;
    std::wstring extractMessage;
    if (!ExtractWithSevenZip(archivePath, tempDir, password, nullptr, cancel,
                             pause, extracted, failed, extractMessage)) {
        fs::remove_all(tempDir, ec);
        message = extractMessage.empty() ? L"无法解包原压缩包。"
                                          : extractMessage;
        return false;
    }

    auto safeRelative = [](const std::wstring& path) {
        std::wstring value = path;
        while (!value.empty() && (value.front() == L'\\' || value.front() == L'/')) {
            value.erase(value.begin());
        }
        std::replace(value.begin(), value.end(), L'/', L'\\');
        return fs::path(value);
    };

    for (const std::wstring& path : removePaths) {
        std::error_code removeEc;
        fs::remove_all(tempDir / safeRelative(path), removeEc);
    }

    if (!renameFrom.empty() && !renameTo.empty()) {
        const fs::path from = tempDir / safeRelative(renameFrom);
        const fs::path to = tempDir / safeRelative(renameTo);
        std::error_code renameEc;
        fs::create_directories(to.parent_path(), renameEc);
        renameEc.clear();
        fs::rename(from, to, renameEc);
        if (renameEc) {
            fs::remove_all(tempDir, ec);
            message = L"无法重命名该条目。";
            return false;
        }
    }

    CompressOptions options;
    for (fs::directory_iterator it(tempDir, ec), end; !ec && it != end;
         it.increment(ec)) {
        options.sources.push_back(it->path());
    }
    options.format = (archiveFormat == ArchiveFormat::Zip)
                         ? CompressFormat::Zip
                         : CompressFormat::SevenZip;
    options.password = password;
    options.outputFile = archivePath + L".rebuild";

    int added = 0;
    std::wstring compressMessage;
    if (!CompressItems(options, nullptr, cancel, pause, added,
                       compressMessage)) {
        fs::remove_all(tempDir, ec);
        DeleteFileW((archivePath + L".rebuild").c_str());
        message = compressMessage;
        return false;
    }

    DeleteFileW(archivePath.c_str());
    if (!MoveFileExW((archivePath + L".rebuild").c_str(), archivePath.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        fs::remove_all(tempDir, ec);
        message = L"无法替换原压缩包。";
        return false;
    }

    fs::remove_all(tempDir, ec);
    message = L"压缩包已更新。";
    return true;
}

bool AppendFilesToArchive(const std::wstring& archivePath,
                          const std::wstring& password,
                          const std::vector<std::wstring>& files,
                          std::wstring& message,
                          const std::wstring& targetPrefix,
                          bool overwrite) {
    message.clear();
    if (files.empty()) {
        message = L"没有要添加的文件。";
        return false;
    }

    const ArchiveFormat format = DetectFormat(archivePath);
    if (format == ArchiveFormat::GZip || format == ArchiveFormat::Xz ||
        format == ArchiveFormat::Iso) {
        message = L"该格式不支持追加压缩。";
        return false;
    }

    if (overwrite) {
        std::vector<ArchiveEntry> entries;
        std::wstring listMessage;
        std::wstring actualPath;
        if (PrepareArchiveSource(archivePath, actualPath, listMessage) &&
            ListArchiveEntries(archivePath, password, entries, listMessage,
                               actualPath)) {
            std::set<std::wstring> newPaths;
            for (const std::wstring& file : files) {
                std::wstring name = targetPrefix;
                if (!name.empty() && name.back() != L'\\') {
                    name += L'\\';
                }
                name += fs::path(file).filename().wstring();
                newPaths.insert(NormalizePath(name));
            }
            bool conflict = false;
            for (const ArchiveEntry& entry : entries) {
                if (!entry.isDir &&
                    newPaths.count(NormalizePath(entry.path)) > 0) {
                    conflict = true;
                    break;
                }
            }
            if (conflict) {
                if (IsSplitFirstVolume(archivePath)) {
                    message = L"分卷压缩包不支持覆盖同名文件。";
                    return false;
                }
                if (format == ArchiveFormat::Zip) {
                    return RebuildZipWithOverwrite(archivePath, password, files,
                                                   targetPrefix, message);
                }
            }
        }
    }

    CInFileStream* inSpec = new CInFileStream;
    CMyComPtr<IInStream> inStream(inSpec);
    if (!inSpec->Open(archivePath)) {
        message = L"无法打开压缩包。";
        return false;
    }

    COpenCallback* openSpec = new COpenCallback;
    openSpec->password = password;
    CMyComPtr<IArchiveOpenCallback> openCb(openSpec);

    const std::vector<const GUID*> clsids = ClsidsForFormat(format);
    if (clsids.empty()) {
        message = L"该格式不支持追加压缩。";
        return false;
    }

    void* object = nullptr;
    if (CreateObject(clsids[0], &IID_IOutArchive, &object) != S_OK ||
        object == nullptr) {
        message = L"该格式不支持追加压缩。";
        return false;
    }

    CMyComPtr<IOutArchive> outArchive;
    outArchive.Attach(reinterpret_cast<IOutArchive*>(object));

    CMyComPtr<IInArchive> inArchive;
    reinterpret_cast<IUnknown*>(object)->QueryInterface(
        IID_IInArchive, reinterpret_cast<void**>(&inArchive));
    if (inArchive == nullptr ||
        inArchive->Open(inStream, nullptr, openCb) != S_OK) {
        message = L"无法打开压缩包（可能已加密或不支持追加）。";
        return false;
    }

    UInt32 oldCount = 0;
    inArchive->GetNumberOfItems(&oldCount);

    const std::wstring tempPath = archivePath + L".tmp";
    CAppendOutStream* outSpec = new CAppendOutStream;
    CMyComPtr<IOutStream> outStream(outSpec);
    if (!outSpec->Create(tempPath)) {
        inArchive->Close();
        message = L"无法创建临时文件。";
        return false;
    }

    CAppendCallback* cbSpec = new CAppendCallback;
    cbSpec->archive = inArchive;
    cbSpec->files = files;
    cbSpec->oldCount = oldCount;
    cbSpec->password = password;
    cbSpec->targetPrefix = targetPrefix;
    cbSpec->overwrite = overwrite;
    for (const std::wstring& file : files) {
        std::wstring name = targetPrefix;
        if (!name.empty() && name.back() != L'\\') {
            name += L'\\';
        }
        name += fs::path(file).filename().wstring();
        cbSpec->newPaths.insert(NormalizePath(name));
    }
    CMyComPtr<IArchiveUpdateCallback> cb(cbSpec);

    const HRESULT hr = outArchive->UpdateItems(
        outStream, oldCount + static_cast<UInt32>(files.size()), cb);
    outStream.Release();
    inArchive->Close();
    inStream.Release();

    if (hr != S_OK) {
        DeleteFileW(tempPath.c_str());
        message = L"追加压缩失败。";
        return false;
    }

    DeleteFileW(archivePath.c_str());
    if (!MoveFileExW(tempPath.c_str(), archivePath.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        message = L"无法替换原压缩包。";
        return false;
    }

    message = L"已添加 " + std::to_wstring(files.size()) + L" 个文件。";
    return true;
}

bool PrepareArchiveSource(const std::wstring& archivePath,
                          std::wstring& actualPath,
                          std::wstring& message) {
    message.clear();
    actualPath = archivePath;

    const std::wstring fileName = fs::path(archivePath).filename().wstring();
    if (EndsWithNoCase(fileName, L".z01")) {
        const fs::path p(archivePath);
        const fs::path zipPath =
            p.parent_path() / (p.stem().wstring() + L".zip");
        std::error_code ec;
        if (fs::exists(zipPath, ec)) {
            actualPath = zipPath.wstring();
        }
        return true;
    }

    if (IsSplitFirstVolume(archivePath)) {
        std::wstring merged;
        if (!JoinSplitVolumes(archivePath, merged)) {
            message = L"无法合并分卷文件。";
            return false;
        }
        actualPath = merged;
    }
    return true;
}

bool ListArchiveEntries(const std::wstring& archivePath,
                        const std::wstring& password,
                        std::vector<ArchiveEntry>& entries,
                        std::wstring& message,
                        const std::wstring& actualPathOverride) {
    entries.clear();
    message.clear();

    std::wstring merged;
    std::wstring path = actualPathOverride;
    if (path.empty()) {
        path = archivePath;
        const std::wstring fileName = fs::path(path).filename().wstring();
        if (EndsWithNoCase(fileName, L".z01")) {
            const fs::path p(path);
            path = (p.parent_path() / (p.stem().wstring() + L".zip")).wstring();
        }
        if (IsSplitFirstVolume(path)) {
            if (!JoinSplitVolumes(path, merged)) {
                message = L"无法合并分卷文件。";
                return false;
            }
            path = merged;
        }
    }
    TempFileCleaner cleaner{merged};

    const ArchiveFormat format = DetectFormat(path);
    const bool useCache = !actualPathOverride.empty();
    std::unique_lock<std::mutex> cacheLock(g_archiveMutex, std::defer_lock);
    if (useCache) {
        cacheLock.lock();
    }
    CMyComPtr<IInArchive> archive;
    CMyComPtr<IInStream> stream;
    const HRESULT openResult =
        useCache ? OpenCachedArchiveLocked(path, format, password, archive,
                                           stream)
                 : OpenEngineArchive(path, format, password, archive, stream);
    if (openResult == S_FALSE) {
        const std::wstring name = fs::path(archivePath).filename().wstring();
        if (EndsWithNoCase(name, L".001")) {
            message = L"无法打开压缩包：分卷文件不完整或格式不支持。";
            return false;
        }
        message = L"需要密码";
        return false;
    }
    if (openResult != S_OK || !archive) {
        message = L"无法打开压缩包：格式不支持或文件损坏。";
        return false;
    }

    UInt32 count = 0;
    archive->GetNumberOfItems(&count);
    for (UInt32 i = 0; i < count; ++i) {
        ArchiveEntry entry;
        PROPVARIANT prop;
        PropVariantInit(&prop);
        if (archive->GetProperty(i, kpidPath, &prop) == S_OK &&
            prop.vt == VT_BSTR && prop.bstrVal != nullptr) {
            entry.path = prop.bstrVal;
        }
        PropVariantClear(&prop);

        PropVariantInit(&prop);
        if (archive->GetProperty(i, kpidSize, &prop) == S_OK &&
            prop.vt == VT_UI8) {
            entry.size = prop.uhVal.QuadPart;
        }
        PropVariantClear(&prop);

        PropVariantInit(&prop);
        if (archive->GetProperty(i, kpidIsDir, &prop) == S_OK &&
            prop.vt == VT_BOOL) {
            entry.isDir = prop.boolVal != VARIANT_FALSE;
        }
        PropVariantClear(&prop);

        entry.index = static_cast<int>(i);
        if (!entry.path.empty()) {
            entries.push_back(entry);
        }
    }
    if (!useCache) {
        archive->Close();
    }

    if (entries.empty()) {
        message = L"压缩包是空的。";
        return false;
    }
    return true;
}

bool ExtractWithSevenZip(const std::wstring& archivePath,
                         const fs::path& outputDir,
                         const std::wstring& password,
                         HWND hwnd,
                         std::atomic<bool>& cancelRequested,
                         std::atomic<bool>& pauseRequested,
                         int& extracted,
                         int& failed,
                         std::wstring& message,
                         ExtractNotify* notify,
                         ConflictPolicy conflictPolicy) {
    extracted = 0;
    failed = 0;
    message.clear();

    const ArchiveFormat format = DetectFormat(archivePath);
    if (format != ArchiveFormat::GZip && format != ArchiveFormat::Xz) {
        const bool stripTopDirectory = (format != ArchiveFormat::Iso);
        return ExtractDirectArchive(archivePath, format, outputDir, password, hwnd,
                                    cancelRequested, pauseRequested, 0, 100,
                                    extracted, failed, message, stripTopDirectory,
                                    std::wstring(), notify, conflictPolicy);
    }

    // 单流压缩：.gz/.xz，以及 .tgz/.tar.gz/.tar.xz/.txz
    const fs::path tempDir = CreateTgzTempDir();
    if (tempDir.empty()) {
        message = L"无法创建临时解压目录。";
        return false;
    }

    int innerExtracted = 0;
    int innerFailed = 0;
    std::wstring innerMessage;

    const bool innerOk = ExtractDirectArchive(
        archivePath, format, tempDir, L"", hwnd,
        cancelRequested, pauseRequested, 0, 50,
        innerExtracted, innerFailed, innerMessage, false, L"inner.bin", notify,
        ConflictPolicy::Overwrite);

    if (!innerOk) {
        std::error_code removeEc;
        fs::remove_all(tempDir, removeEc);
        message = innerMessage;
        return false;
    }

    const fs::path innerArchive = FindFirstRegularFile(tempDir);
    if (innerArchive.empty()) {
        std::error_code removeEc;
        fs::remove_all(tempDir, removeEc);
        message = L"压缩流中没有找到可解出的文件。";
        return false;
    }

    if (IsTarFile(innerArchive)) {
        int tarExtracted = 0;
        int tarFailed = 0;
        std::wstring tarMessage;

        const bool tarOk = ExtractDirectArchive(
            innerArchive.wstring(), ArchiveFormat::Tar, outputDir, L"", hwnd,
            cancelRequested, pauseRequested, 50, 50,
            tarExtracted, tarFailed, tarMessage, true, std::wstring(), notify,
            conflictPolicy);

        extracted = tarExtracted;
        failed = innerFailed + tarFailed;

        std::error_code removeEc;
        fs::remove_all(tempDir, removeEc);

        message = tarMessage;
        return tarOk;
    }

    std::error_code ec;
    fs::create_directories(outputDir, ec);
    const std::wstring targetName =
        StripCompressionSuffix(fs::path(archivePath).filename().wstring());
    const fs::path target = outputDir / targetName;
    fs::copy_file(innerArchive, target, fs::copy_options::overwrite_existing, ec);

    std::error_code removeEc;
    fs::remove_all(tempDir, removeEc);

    if (ec) {
        extracted = 0;
        failed = innerFailed + 1;
        message = L"解压失败：无法写出解出的文件。";
        return false;
    }

    extracted = 1;
    failed = innerFailed;
    message = L"解压完成：成功 1 个文件。";
    if (hwnd != nullptr) {
        PostMessageW(hwnd, WM_APP + 1, 100, 0);
    }
    return true;
}
void ReleaseArchiveCache() {
    std::lock_guard<std::mutex> lock(g_archiveMutex);
    if (g_cachedArchive.archive != nullptr) {
        g_cachedArchive.archive->Close();
        g_cachedArchive.archive.Release();
        g_cachedArchive.stream.Release();
    }
    g_cachedArchive.path.clear();
    g_cachedArchive.password.clear();
}