#include "compress_engine.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <system_error>
#include <vector>

#include <propidl.h>
#include <oleauto.h>

#include "Common/MyWindows.h"
#include "Common/MyCom.h"
#include "7zip/Archive/IArchive.h"
#include "7zip/IStream.h"
#include "7zip/IProgress.h"
#include "7zip/IPassword.h"
#include "7zip/PropID.h"

namespace fs = std::filesystem;

extern "C" HRESULT __stdcall CreateObject(const GUID* clsid, const GUID* iid, void** outObject);

extern int zpaq_main(int argc, const char** argv);

namespace {

int SplitOutputVolumes(const fs::path& file, unsigned long long volumeSize) {
    if (volumeSize == 0) {
        return 0;
    }
    std::error_code ec;
    const unsigned long long total = fs::file_size(file, ec);
    if (ec || total <= volumeSize) {
        return 0;
    }

    HANDLE in = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        return -1;
    }

    std::vector<char> buffer(1 << 20);
    unsigned long long remaining = total;
    int index = 1;
    bool ok = true;
    while (remaining > 0) {
        std::wstring suffix = std::to_wstring(index);
        while (suffix.size() < 3) {
            suffix = L"0" + suffix;
        }
        const fs::path part = file.wstring() + L"." + suffix;
        HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) {
            ok = false;
            break;
        }
        unsigned long long writtenTotal = 0;
        while (writtenTotal < volumeSize && remaining > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<unsigned long long>(
                buffer.size(), volumeSize - writtenTotal));
            DWORD readBytes = 0;
            if (!ReadFile(in, buffer.data(), chunk, &readBytes, nullptr) ||
                readBytes == 0) {
                ok = false;
                break;
            }
            DWORD written = 0;
            if (!WriteFile(out, buffer.data(), readBytes, &written, nullptr) ||
                written != readBytes) {
                ok = false;
                break;
            }
            writtenTotal += readBytes;
            remaining -= readBytes;
        }
        CloseHandle(out);
        if (!ok) {
            break;
        }
        ++index;
    }
    CloseHandle(in);

    if (!ok || remaining > 0) {
        return -1;
    }
    DeleteFileW(file.c_str());
    return index - 1;
}

const GUID kClsidZip = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0x01, 0x00, 0x00}
};
const GUID kClsid7z = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0x07, 0x00, 0x00}
};
const GUID kClsidTar = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0xEE, 0x00, 0x00}
};
const GUID kClsidGZip = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0xEF, 0x00, 0x00}
};
const GUID kClsidXz = {
    0x23170F69, 0x40C1, 0x278A,
    {0x10, 0x00, 0x00, 0x01, 0x10, 0x0C, 0x00, 0x00}
};

class COutStream final : public IOutStream, public CMyUnknownImp {
    Z7_COM_UNKNOWN_IMP_2(ISequentialOutStream, IOutStream)

public:
    std::atomic<bool>* pauseRequested = nullptr;
    std::atomic<bool>* cancelRequested = nullptr;

    ~COutStream() {
        Close();
    }

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
        if (cancelRequested != nullptr && cancelRequested->load()) {
            return E_ABORT;
        }
        if (pauseRequested != nullptr) {
            while (pauseRequested->load() &&
                   (cancelRequested == nullptr || !cancelRequested->load())) {
                Sleep(20);
            }
        }
        if (cancelRequested != nullptr && cancelRequested->load()) {
            return E_ABORT;
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
        LARGE_INTEGER saved{};
        saved.QuadPart = static_cast<LONGLONG>(_position);
        const UInt64 restore = _position;

        LARGE_INTEGER target{};
        target.QuadPart = static_cast<LONGLONG>(newSize);
        if (!SetFilePointerEx(_handle, target, nullptr, FILE_BEGIN)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        const BOOL ok = SetEndOfFile(_handle);
        const DWORD endError = ok ? ERROR_SUCCESS : GetLastError();

        SetFilePointerEx(_handle, saved, nullptr, FILE_BEGIN);
        _position = restore;

        if (!ok) {
            return HRESULT_FROM_WIN32(endError);
        }
        return S_OK;
    }

private:
    HANDLE _handle = INVALID_HANDLE_VALUE;
    UInt64 _position = 0;
};

class CInFileStream final : public IInStream, public CMyUnknownImp {
    Z7_COM_UNKNOWN_IMP_2(ISequentialInStream, IInStream)

public:
    ~CInFileStream() {
        Close();
    }

    bool Open(const std::wstring& path) {
        Close();
        _handle = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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
        DWORD read = 0;
        if (!::ReadFile(_handle, data, size, &read, nullptr)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (processedSize != nullptr) {
            *processedSize = read;
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
        if (newPosition != nullptr) {
            *newPosition = static_cast<UInt64>(result.QuadPart);
        }
        return S_OK;
    }

private:
    HANDLE _handle = INVALID_HANDLE_VALUE;
};

struct SourceItem {
    fs::path fullPath;
    fs::path archivePath;
    bool isDir = false;
    UInt64 size = 0;
    FILETIME mtime{};
    DWORD attrib = 0;
};

void CollectItems(const fs::path& current, const fs::path& base,
                  std::vector<SourceItem>& items) {
    std::error_code ec;
    SourceItem item;
    item.fullPath = current;
    item.archivePath = fs::relative(current, base, ec);
    if (ec || item.archivePath.empty()) {
        item.archivePath = current.filename();
    }

    if (fs::is_directory(current, ec)) {
        item.isDir = true;
        item.attrib = GetFileAttributesW(current.c_str());
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(current.c_str(), GetFileExInfoStandard, &data)) {
            item.mtime = data.ftLastWriteTime;
        }
        items.push_back(item);

        for (fs::directory_iterator it(current, ec), end;
             !ec && it != end; it.increment(ec)) {
            CollectItems(it->path(), base, items);
        }
        return;
    }

    std::error_code sizeEc;
    item.size = fs::file_size(current, sizeEc);
    if (sizeEc) {
        item.size = 0;
    }
    item.attrib = GetFileAttributesW(current.c_str());
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(current.c_str(), GetFileExInfoStandard, &data)) {
        item.mtime = data.ftLastWriteTime;
    }
    items.push_back(item);
}

class CUpdateCallback final : public IArchiveUpdateCallback,
                              public ICryptoGetTextPassword2,
                              public CMyUnknownImp {
    Z7_IFACES_IMP_UNK_2(IArchiveUpdateCallback, ICryptoGetTextPassword2)
    Z7_IFACE_COM7_IMP(IProgress)

public:
    std::vector<SourceItem>* items = nullptr;
    HWND hwnd = nullptr;
    std::atomic<bool>* cancelRequested = nullptr;
    std::atomic<bool>* pauseRequested = nullptr;
    UInt64 totalBytes = 0;
    std::wstring password;

    };

Z7_COM7F_IMF(CUpdateCallback::GetUpdateItemInfo(UInt32 index, Int32* newData,
                                                Int32* newProperties,
                                                UInt32* indexInArchive)) {
    if (items == nullptr || index >= items->size()) {
        return E_INVALIDARG;
    }
    if (newData != nullptr) {
        *newData = 1;
    }
    if (newProperties != nullptr) {
        *newProperties = 1;
    }
    if (indexInArchive != nullptr) {
        *indexInArchive = static_cast<UInt32>(static_cast<Int32>(-1));
    }
    return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::GetProperty(UInt32 index, PROPID propID,
                                          PROPVARIANT* value)) {
    PropVariantInit(value);
    if (items == nullptr || index >= items->size()) {
        return E_INVALIDARG;
    }
    const SourceItem& item = (*items)[index];

    switch (propID) {
    case kpidPath:
        value->vt = VT_BSTR;
        value->bstrVal = SysAllocString(item.archivePath.wstring().c_str());
        if (value->bstrVal == nullptr) {
            return E_OUTOFMEMORY;
        }
        break;
    case kpidIsDir:
        value->vt = VT_BOOL;
        value->boolVal = item.isDir ? VARIANT_TRUE : VARIANT_FALSE;
        break;
    case kpidSize:
        value->vt = VT_UI8;
        value->uhVal.QuadPart = item.size;
        break;
    case kpidAttrib:
        value->vt = VT_UI4;
        value->ulVal = item.attrib;
        break;
    case kpidMTime:
        value->vt = VT_FILETIME;
        value->filetime = item.mtime;
        break;
    case kpidIsAnti:
        value->vt = VT_BOOL;
        value->boolVal = VARIANT_FALSE;
        break;
    default:
        break;
    }
    return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::GetStream(UInt32 index,
                                        ISequentialInStream** inStream)) {
    if (inStream != nullptr) {
        *inStream = nullptr;
    }
    if (items == nullptr || index >= items->size() || inStream == nullptr) {
        return E_INVALIDARG;
    }
    const SourceItem& item = (*items)[index];
    if (item.isDir) {
        return S_OK;
    }

    CInFileStream* streamSpec = new CInFileStream;
    CMyComPtr<ISequentialInStream> stream(streamSpec);
    if (!streamSpec->Open(item.fullPath.wstring())) {
        return E_FAIL;
    }
    *inStream = stream.Detach();
    return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::SetOperationResult(Int32)) {
    return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::CryptoGetTextPassword2(Int32* passwordIsDefined,
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

Z7_COM7F_IMF(CUpdateCallback::SetTotal(UInt64 total)) {
    totalBytes = total;
    return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::SetCompleted(const UInt64* completeValue)) {
    if (cancelRequested != nullptr && cancelRequested->load()) {
        return E_ABORT;
    }
    if (pauseRequested != nullptr) {
        while (pauseRequested->load() &&
               (cancelRequested == nullptr || !cancelRequested->load())) {
            Sleep(20);
        }
    }
    if (cancelRequested != nullptr && cancelRequested->load()) {
        return E_ABORT;
    }

    if (completeValue != nullptr && totalBytes > 0 && hwnd != nullptr) {
        int percent = static_cast<int>((*completeValue * 100) / totalBytes);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        PostMessageW(hwnd, WM_APP + 1, static_cast<WPARAM>(percent), 0);
    }
    return S_OK;
}

bool RunUpdate(const fs::path& outputPath, const GUID& clsid,
               const std::vector<SourceItem>& items,
               const CompressOptions& options, HWND hwnd,
               std::atomic<bool>& cancelRequested,
               std::atomic<bool>& pauseRequested, std::wstring& message) {
    void* object = nullptr;
    HRESULT result = CreateObject(&clsid, &IID_IOutArchive, &object);
    if (result != S_OK || object == nullptr) {
        message = L"该压缩格式不受支持。";
        return false;
    }

    CMyComPtr<IOutArchive> outArchive;
    outArchive.Attach(reinterpret_cast<IOutArchive*>(object));

    CMyComPtr<ISetProperties> setProperties;
    if (outArchive->QueryInterface(IID_ISetProperties,
                                   reinterpret_cast<void**>(&setProperties)) == S_OK &&
        setProperties != nullptr) {
        std::vector<std::wstring> nameStorage;
        std::vector<PROPVARIANT> values;

        auto addUInt = [&](const wchar_t* name, ULONG value) {
            nameStorage.push_back(name);
            PROPVARIANT prop;
            PropVariantInit(&prop);
            prop.vt = VT_UI4;
            prop.ulVal = value;
            values.push_back(prop);
        };
        auto addString = [&](const wchar_t* name, const std::wstring& value) {
            nameStorage.push_back(name);
            PROPVARIANT prop;
            PropVariantInit(&prop);
            prop.vt = VT_BSTR;
            prop.bstrVal = SysAllocString(value.c_str());
            values.push_back(prop);
        };
        auto addBool = [&](const wchar_t* name, bool value) {
            nameStorage.push_back(name);
            PROPVARIANT prop;
            PropVariantInit(&prop);
            prop.vt = VT_BOOL;
            prop.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
            values.push_back(prop);
        };

        if (options.level >= 0) {
            addUInt(L"x", static_cast<ULONG>(options.level));
        }
        if (!options.method.empty() && clsid != kClsidTar &&
            clsid != kClsidGZip && clsid != kClsidXz) {
            addString(L"m", options.method);
        }
        if (options.threads > 0) {
            addUInt(L"mt", static_cast<ULONG>(options.threads));
        }
        if (!options.password.empty()) {
            if (clsid == kClsidZip) {
                addString(L"em", L"AES256");
            } else if (clsid == kClsid7z) {
                addBool(L"he", true);
            }
        }

        if (!values.empty()) {
            std::vector<const wchar_t*> namePtrs;
            namePtrs.reserve(nameStorage.size());
            for (const std::wstring& name : nameStorage) {
                namePtrs.push_back(name.c_str());
            }
            setProperties->SetProperties(namePtrs.data(), values.data(),
                                         static_cast<UInt32>(values.size()));
            for (PROPVARIANT& prop : values) {
                PropVariantClear(&prop);
            }
        }
    }

    COutStream* outSpec = new COutStream;
    CMyComPtr<IOutStream> outStream(outSpec);
    outSpec->cancelRequested = &cancelRequested;
    outSpec->pauseRequested = &pauseRequested;
    if (!outSpec->Create(outputPath.wstring())) {
        message = L"无法创建输出文件。";
        return false;
    }

    CUpdateCallback* callbackSpec = new CUpdateCallback;
    CMyComPtr<IArchiveUpdateCallback> callback(callbackSpec);
    callbackSpec->items = const_cast<std::vector<SourceItem>*>(&items);
    callbackSpec->hwnd = hwnd;
    callbackSpec->cancelRequested = &cancelRequested;
    callbackSpec->pauseRequested = &pauseRequested;
    callbackSpec->password = options.password;

    result = outArchive->UpdateItems(outStream, static_cast<UInt32>(items.size()),
                                     callback);
    outStream.Release();

    if (cancelRequested.load()) {
        message = L"已取消压缩。";
        return false;
    }
    if (result != S_OK) {
        message = L"压缩失败：源文件不可读或目标文件被占用。";
        return false;
    }
    return true;
}

fs::path CreateTempTarPath() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) {
        return fs::path();
    }
    return base / (L"PopZip_pack_" + std::to_wstring(GetCurrentProcessId()) +
                   L"_" + std::to_wstring(GetTickCount64()) + L".tar");
}

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

bool CompressZpaq(const CompressOptions& options, HWND hwnd,
                  std::atomic<bool>& cancelRequested,
                  std::atomic<bool>& pauseRequested,
                  std::wstring& message) {
    if (options.sources.empty()) {
        message = L"请先添加文件或文件夹。";
        return false;
    }
    if (hwnd != nullptr) {
        PostMessageW(hwnd, WM_APP + 1, 0, 0);
    }

    const std::string archiveUtf8 = Utf8FromWide(options.outputFile.wstring());
    const std::string passwordUtf8 = Utf8FromWide(options.password);
    const std::string method =
        "-m" + std::to_string(std::max(1, std::min(5, options.level / 2 + 1)));

    for (size_t i = 0; i < options.sources.size(); ++i) {
        if (cancelRequested.load()) {
            message = L"已取消压缩。";
            return false;
        }

        const fs::path& source = options.sources[i];
        const fs::path parent = source.parent_path();
        const std::wstring name = source.filename().wstring();
        if (parent.empty() || name.empty()) {
            message = L"无法确定压缩源路径。";
            return false;
        }

        const std::wstring previousDir = fs::current_path().wstring();
        std::error_code cwdEc;
        fs::current_path(parent, cwdEc);
        if (cwdEc) {
            message = L"无法切换工作目录。";
            return false;
        }

        const std::string nameUtf8 = Utf8FromWide(name);
        std::vector<const char*> args;
        args.push_back("PopZip");
        args.push_back("add");
        args.push_back(archiveUtf8.c_str());
        args.push_back(nameUtf8.c_str());
        args.push_back("-force");
        args.push_back(method.c_str());
        if (!passwordUtf8.empty()) {
            args.push_back("-key");
            args.push_back(passwordUtf8.c_str());
        }

        const int result = zpaq_main(static_cast<int>(args.size()), args.data());

        std::error_code restoreEc;
        fs::current_path(previousDir, restoreEc);

        if (cancelRequested.load()) {
            message = L"已取消压缩。";
            return false;
        }
        if (result != 0) {
            message = L"ZPAQ 压缩失败。";
            return false;
        }

        if (hwnd != nullptr) {
            const int percent = static_cast<int>(
                ((i + 1) * 100) / options.sources.size());
            PostMessageW(hwnd, WM_APP + 1,
                         static_cast<WPARAM>(std::min(percent, 100)), 0);
        }
    }
    return true;
}

} // namespace

const wchar_t* CompressFormatExtension(CompressFormat format) {
    switch (format) {
    case CompressFormat::Zip:
        return L".zip";
    case CompressFormat::SevenZip:
        return L".7z";
    case CompressFormat::Tar:
        return L".tar";
    case CompressFormat::Gz:
        return L".gz";
    case CompressFormat::Xz:
        return L".xz";
    case CompressFormat::Tgz:
        return L".tgz";
    case CompressFormat::TarXz:
        return L".tar.xz";
    case CompressFormat::Zpaq:
        return L".zpaq";
    }
    return L".zip";
}

bool CompressItems(const CompressOptions& options, HWND hwnd,
                   std::atomic<bool>& cancelRequested,
                   std::atomic<bool>& pauseRequested,
                   int& addedFiles, std::wstring& message) {
    addedFiles = 0;
    message.clear();

    if (options.sources.empty()) {
        message = L"请先添加要压缩的文件或文件夹。";
        return false;
    }

    if (options.format == CompressFormat::Zpaq) {
        const bool ok = CompressZpaq(options, hwnd, cancelRequested,
                                     pauseRequested, message);
        if (ok) {
            const int volumes =
                SplitOutputVolumes(options.outputFile, options.volumeSize);
            if (volumes > 1) {
                message += L" 已切分为 " + std::to_wstring(volumes) + L" 卷。";
            }
        }
        return ok;
    }

    std::vector<SourceItem> items;
    for (const fs::path& source : options.sources) {
        std::error_code ec;
        if (fs::is_directory(source, ec)) {
            CollectItems(source, source.parent_path(), items);
            continue;
        }
        if (!fs::is_regular_file(source, ec)) {
            continue;
        }

        SourceItem item;
        item.fullPath = source;
        item.archivePath = source.filename();
        std::error_code sizeEc;
        item.size = fs::file_size(source, sizeEc);
        if (sizeEc) {
            item.size = 0;
        }
        item.attrib = GetFileAttributesW(source.c_str());
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &data)) {
            item.mtime = data.ftLastWriteTime;
        }
        items.push_back(item);
    }

    for (const SourceItem& item : items) {
        if (!item.isDir) {
            ++addedFiles;
        }
    }
    if (items.empty()) {
        message = L"选中的内容为空，没有可压缩的文件。";
        return false;
    }

    if (options.format == CompressFormat::Tgz ||
        options.format == CompressFormat::TarXz ||
        options.format == CompressFormat::Gz ||
        options.format == CompressFormat::Xz) {
        const fs::path tempTar = CreateTempTarPath();
        if (tempTar.empty()) {
            message = L"无法创建临时打包文件。";
            return false;
        }

        std::wstring tarMessage;
        if (!RunUpdate(tempTar, kClsidTar, items, options, hwnd,
                       cancelRequested, pauseRequested, tarMessage)) {
            std::error_code removeEc;
            fs::remove(tempTar, removeEc);
            message = tarMessage;
            return false;
        }

        std::vector<SourceItem> single(1);
        single[0].fullPath = tempTar;
        single[0].archivePath = tempTar.filename();
        single[0].isDir = false;
        std::error_code sizeEc;
        single[0].size = fs::file_size(tempTar, sizeEc);
        if (sizeEc) {
            single[0].size = 0;
        }

        const bool useXz = (options.format == CompressFormat::TarXz ||
                            options.format == CompressFormat::Xz);
        const GUID& streamClsid = useXz ? kClsidXz : kClsidGZip;

        std::wstring streamMessage;
        const bool ok = RunUpdate(options.outputFile, streamClsid, single,
                                  options, hwnd, cancelRequested,
                                  pauseRequested, streamMessage);

        std::error_code removeEc;
        fs::remove(tempTar, removeEc);

        if (!ok) {
            message = streamMessage;
            return false;
        }
        message = L"压缩完成：成功添加 " + std::to_wstring(addedFiles) +
                  L" 个文件。";
        const int volumes =
            SplitOutputVolumes(options.outputFile, options.volumeSize);
        if (volumes > 1) {
            message += L" 已切分为 " + std::to_wstring(volumes) + L" 卷。";
        }
        return true;
    }

    const GUID* clsid = &kClsidZip;
    if (options.format == CompressFormat::SevenZip) {
        clsid = &kClsid7z;
    } else if (options.format == CompressFormat::Tar) {
        clsid = &kClsidTar;
    }

    std::wstring updateMessage;
    if (!RunUpdate(options.outputFile, *clsid, items, options, hwnd,
                   cancelRequested, pauseRequested, updateMessage)) {
        message = updateMessage;
        return false;
    }

    message = L"压缩完成：成功添加 " + std::to_wstring(addedFiles) +
              L" 个文件。";
    const int volumes =
        SplitOutputVolumes(options.outputFile, options.volumeSize);
    if (volumes > 1) {
        message += L" 已切分为 " + std::to_wstring(volumes) + L" 卷。";
    }
    return true;
}