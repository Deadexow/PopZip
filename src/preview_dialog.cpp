#include "preview_dialog.h"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <mfplay.h>
#include <mferror.h>
#include <shlobj.h>
#include <ole2.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "confirm_dialog.h"
#include "password_dialog.h"
#include "rename_dialog.h"
#include "sevenzip_engine.h"
#include "sky.h"
#include "theme.h"

extern int g_theme;

namespace fs = std::filesystem;

namespace {

constexpr int kWidth = 1000;
constexpr int kHeight = 700;
constexpr int kTitleHeight = 46;
constexpr int kToolTop = 56;
constexpr int kToolBottom = 96;
constexpr int kListTop = 108;
constexpr int kRowHeight = 28;
constexpr int kListLeft = 20;
constexpr int kPreviewTop = 108;

struct DisplayItem {
    std::wstring name;
    bool isDir = false;
    int entryIndex = -1;
    unsigned long long size = 0;
};

struct PreviewState {
    std::wstring path;
    std::wstring actualArchivePath;
    std::wstring password;
    std::vector<ArchiveEntry> entries;
    std::vector<DisplayItem> display;
    std::wstring currentPrefix;
    std::wstring message;
    std::wstring search;
    bool searchActive = false;
    std::wstring previewFile;
    bool previewIsTemp = false;
    HWND surface = nullptr;
    HBITMAP image = nullptr;
    int imageWidth = 0;
    int imageHeight = 0;
    IMFPMediaPlayer* player = nullptr;
    bool playing = false;
    bool mediaMode = false;
    double duration = 0.0;
    double position = 0.0;
    bool seeking = false;
    std::atomic<bool> loading{false};
    int dragIndex = -1;
    POINT dragStart{};
    HWND hwnd = nullptr;
    int scroll = 0;
    int selected = -1;
    std::set<int> multiSelected;
    int hover = 0;
    bool trackingMouse = false;
    SkyState sky;
    SkyState surfaceSky;
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
};

bool mfStarted = false;

std::mutex g_loadMutex;
std::wstring g_loadTemp;
std::wstring g_loadMessage;
bool g_loadOk = false;

std::mutex g_prepareMutex;
std::wstring g_prepareActual;
std::wstring g_prepareMessage;
bool g_prepareOk = false;

std::mutex g_testMutex;
std::wstring g_testMessage;
bool g_testOk = false;

class CEnumFormatEtc final : public IEnumFORMATETC {
public:
    explicit CEnumFormatEtc(const FORMATETC& format) : format_(format) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
            *ppv = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }

    ULONG STDMETHODCALLTYPE Release() override {
        if (--ref_ == 0) {
            delete this;
            return 0;
        }
        return ref_;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* out,
                                   ULONG* fetched) override {
        if (fetched != nullptr) {
            *fetched = 0;
        }
        if (index_ >= 1 || count == 0) {
            return S_FALSE;
        }
        if (out != nullptr) {
            out[0] = format_;
        }
        index_ = 1;
        if (fetched != nullptr) {
            *fetched = 1;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        index_ += count;
        return S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Reset() override {
        index_ = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        auto* clone = new CEnumFormatEtc(format_);
        clone->index_ = index_;
        *out = clone;
        return S_OK;
    }

private:
    FORMATETC format_{};
    ULONG index_ = 0;
    ULONG ref_ = 1;
};

class CFileDataObject final : public IDataObject {
public:
    explicit CFileDataObject(const std::vector<std::wstring>& files)
        : files_(files) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }

    ULONG STDMETHODCALLTYPE Release() override {
        if (--ref_ == 0) {
            delete this;
            return 0;
        }
        return ref_;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fmt,
                                      STGMEDIUM* medium) override {
        if (fmt == nullptr || medium == nullptr) {
            return E_INVALIDARG;
        }
        if (fmt->cfFormat != CF_HDROP || (fmt->tymed & TYMED_HGLOBAL) == 0) {
            return DV_E_FORMATETC;
        }

        size_t totalChars = 1;
        for (const std::wstring& file : files_) {
            totalChars += file.size() + 1;
        }
        const size_t bytes = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GHND, bytes);
        if (mem == nullptr) {
            return E_OUTOFMEMORY;
        }
        auto* drop = static_cast<DROPFILES*>(GlobalLock(mem));
        if (drop == nullptr) {
            GlobalFree(mem);
            return E_OUTOFMEMORY;
        }
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        wchar_t* dest = reinterpret_cast<wchar_t*>(
            reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
        for (const std::wstring& file : files_) {
            memcpy(dest, file.c_str(), file.size() * sizeof(wchar_t));
            dest += file.size();
            *dest++ = 0;
        }
        *dest = 0;
        GlobalUnlock(mem);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = mem;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*,
                                          STGMEDIUM*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fmt) override {
        if (fmt == nullptr) {
            return E_INVALIDARG;
        }
        return (fmt->cfFormat == CF_HDROP && (fmt->tymed & TYMED_HGLOBAL))
                   ? S_OK
                   : DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,
                                                    FORMATETC* out) override {
        if (out != nullptr) {
            out->ptd = nullptr;
        }
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction,
                                            IEnumFORMATETC** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        FORMATETC format{};
        format.cfFormat = CF_HDROP;
        format.dwAspect = DVASPECT_CONTENT;
        format.lindex = -1;
        format.tymed = TYMED_HGLOBAL;
        *out = new CEnumFormatEtc(format);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*,
                                      DWORD*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return E_NOTIMPL;
    }

private:
    std::vector<std::wstring> files_;
    ULONG ref_ = 1;
};

class CDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }

    ULONG STDMETHODCALLTYPE Release() override {
        if (--ref_ == 0) {
            delete this;
            return 0;
        }
        return ref_;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed,
                                                DWORD keyState) override {
        if (escapePressed) {
            return DRAGDROP_S_CANCEL;
        }
        if ((keyState & MK_LBUTTON) == 0) {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    ULONG ref_ = 1;
};

bool EndsWithLocal(const std::wstring& text, const wchar_t* suffix) {
    const size_t length = wcslen(suffix);
    if (text.size() < length) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (std::towlower(text[text.size() - length + i]) !=
            std::towlower(suffix[i])) {
            return false;
        }
    }
    return true;
}

bool IsImagePath(const std::wstring& path) {
    return EndsWithLocal(path, L".jpg") || EndsWithLocal(path, L".jpeg") ||
           EndsWithLocal(path, L".png") || EndsWithLocal(path, L".bmp") ||
           EndsWithLocal(path, L".gif") || EndsWithLocal(path, L".tif") ||
           EndsWithLocal(path, L".tiff") || EndsWithLocal(path, L".webp") ||
           EndsWithLocal(path, L".ico");
}

bool IsAudioPath(const std::wstring& path) {
    return EndsWithLocal(path, L".mp3") || EndsWithLocal(path, L".wav") ||
           EndsWithLocal(path, L".flac") || EndsWithLocal(path, L".m4a") ||
           EndsWithLocal(path, L".aac") || EndsWithLocal(path, L".ogg") ||
           EndsWithLocal(path, L".wma");
}

bool IsVideoPath(const std::wstring& path) {
    return EndsWithLocal(path, L".mp4") || EndsWithLocal(path, L".m4v") ||
           EndsWithLocal(path, L".mkv") || EndsWithLocal(path, L".avi") ||
           EndsWithLocal(path, L".mov") || EndsWithLocal(path, L".wmv") ||
           EndsWithLocal(path, L".webm") || EndsWithLocal(path, L".flv") ||
           EndsWithLocal(path, L".ts") || EndsWithLocal(path, L".mpg") ||
           EndsWithLocal(path, L".mpeg");
}

std::wstring FormatSize(unsigned long long size) {
    if (size >= 1024ull * 1024 * 1024) {
        return std::to_wstring(size / (1024ull * 1024 * 1024)) + L" GB";
    }
    if (size >= 1024ull * 1024) {
        return std::to_wstring(size / (1024ull * 1024)) + L" MB";
    }
    if (size >= 1024ull) {
        return std::to_wstring(size / 1024ull) + L" KB";
    }
    return std::to_wstring(size) + L" B";
}

int ClientWidth(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    return client.right;
}

int ClientHeight(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    return client.bottom;
}

int PreviewRight(HWND hwnd) { return ClientWidth(hwnd) - 20; }
int PreviewLeft(HWND hwnd) { return ClientWidth(hwnd) - 360; }
int PreviewBottom(HWND hwnd) { return ClientHeight(hwnd) - 84; }
int ListRight(HWND hwnd) { return PreviewLeft(hwnd) - 20; }

int VisibleRows(HWND hwnd) {
    const int available = PreviewBottom(hwnd) - kListTop - 24;
    return available > 0 ? available / kRowHeight : 1;
}

RECT CloseRect(HWND hwnd) {
    const int width = ClientWidth(hwnd);
    return RECT{width - 46, 0, width, kTitleHeight};
}

RECT BackRect() {
    return RECT{20, kToolTop + 2, 120, kToolBottom - 2};
}

RECT OpenRect(HWND hwnd) {
    const int width = ClientWidth(hwnd);
    return RECT{width - 200, kToolTop + 2, width - 20, kToolBottom - 2};
}

RECT TestRect(HWND hwnd) {
    const int width = ClientWidth(hwnd);
    return RECT{width - 318, kToolTop + 2, width - 210, kToolBottom - 2};
}

RECT SearchRect(HWND hwnd) {
    const int width = ClientWidth(hwnd);
    return RECT{width - 545, kToolTop + 2, width - 330, kToolBottom - 2};
}

RECT SurfaceRect(HWND hwnd) {
    return RECT{PreviewLeft(hwnd), kPreviewTop, PreviewRight(hwnd),
                PreviewBottom(hwnd)};
}

void FillRounded(HDC dc, const RECT& rc, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HRGN region = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1,
                                     rc.bottom + 1, radius, radius);
    FillRgn(dc, region, brush);
    DeleteObject(region);
    DeleteObject(brush);
}

void StrokeRounded(HDC dc, const RECT& rc, int radius, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawTextIn(HDC dc, HFONT font, COLORREF color, const RECT& rc,
                const std::wstring& text, UINT flags) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), -1, const_cast<RECT*>(&rc), flags);
}

void DestroyImage(PreviewState& state) {
    if (state.image != nullptr) {
        DeleteObject(state.image);
        state.image = nullptr;
    }
    state.imageWidth = 0;
    state.imageHeight = 0;
}

void StopPlayer(PreviewState& state) {
    if (state.player != nullptr) {
        state.player->Stop();
        state.player->Shutdown();
        state.player->Release();
        state.player = nullptr;
    }
    state.playing = false;
    state.mediaMode = false;
    state.duration = 0.0;
    state.position = 0.0;
    if (state.hwnd != nullptr) {
        KillTimer(state.hwnd, 1);
    }
}

void RemovePreviewFile(PreviewState& state) {
    if (state.previewIsTemp && !state.previewFile.empty()) {
        std::error_code ec;
        fs::remove(state.previewFile, ec);
    }
    state.previewFile.clear();
    state.previewIsTemp = false;
}

std::wstring ParentPrefix(const std::wstring& prefix) {
    if (prefix.empty()) {
        return L"";
    }
    std::wstring value = prefix;
    if (!value.empty() && value.back() == L'\\') {
        value.pop_back();
    }
    const size_t pos = value.rfind(L'\\');
    if (pos == std::wstring::npos) {
        return L"";
    }
    return value.substr(0, pos + 1);
}

void RefreshDisplay(PreviewState& state) {
    state.display.clear();
    state.scroll = 0;
    state.selected = -1;
    state.multiSelected.clear();

    std::map<std::wstring, bool> directories;
    std::vector<DisplayItem> files;

    for (const ArchiveEntry& entry : state.entries) {
        if (entry.path.size() <= state.currentPrefix.size()) {
            continue;
        }
        if (entry.path.compare(0, state.currentPrefix.size(),
                               state.currentPrefix) != 0) {
            continue;
        }
        const std::wstring rest = entry.path.substr(state.currentPrefix.size());
        if (rest.empty()) {
            continue;
        }
        const size_t pos = rest.find(L'\\');
        if (pos == std::wstring::npos) {
            if (!entry.isDir) {
                DisplayItem item;
                item.name = rest;
                item.isDir = false;
                item.entryIndex = entry.index;
                item.size = entry.size;
                files.push_back(item);
            } else {
                directories[rest] = true;
            }
        } else {
            directories[rest.substr(0, pos)] = true;
        }
    }

    for (const auto& dir : directories) {
        if (!state.search.empty() &&
            dir.first.find(state.search) == std::wstring::npos) {
            continue;
        }
        DisplayItem item;
        item.name = dir.first;
        item.isDir = true;
        state.display.push_back(item);
    }
    std::sort(state.display.begin(), state.display.end(),
              [](const DisplayItem& a, const DisplayItem& b) {
                  return std::towlower(a.name[0]) < std::towlower(b.name[0]);
              });
    std::sort(files.begin(), files.end(),
              [](const DisplayItem& a, const DisplayItem& b) {
                  return std::towlower(a.name[0]) < std::towlower(b.name[0]);
              });
    for (const DisplayItem& item : files) {
        if (!state.search.empty() &&
            item.name.find(state.search) == std::wstring::npos) {
            continue;
        }
        state.display.push_back(item);
    }
}

HBITMAP LoadImageFile(const std::wstring& path, int* width, int* height) {
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                  reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
        return nullptr;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad,
                                            &decoder);
    if (FAILED(hr) || decoder == nullptr) {
        factory->Release();
        return nullptr;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || frame == nullptr) {
        decoder->Release();
        factory->Release();
        return nullptr;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || converter == nullptr) {
        frame->Release();
        decoder->Release();
        factory->Release();
        return nullptr;
    }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGR,
                               WICBitmapDitherTypeNone, nullptr, 0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        return nullptr;
    }

    UINT w = 0;
    UINT h = 0;
    hr = converter->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        return nullptr;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(w);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(h);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits,
                                      nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        return nullptr;
    }

    const UINT stride = w * 4;
    hr = converter->CopyPixels(nullptr, stride, stride * h,
                               static_cast<BYTE*>(bits));
    converter->Release();
    frame->Release();
    decoder->Release();
    factory->Release();

    if (FAILED(hr)) {
        DeleteObject(bitmap);
        return nullptr;
    }

    if (width != nullptr) *width = static_cast<int>(w);
    if (height != nullptr) *height = static_cast<int>(h);
    return bitmap;
}

void StartMedia(PreviewState& state) {
    if (!mfStarted) {
        if (FAILED(MFStartup(MF_VERSION))) {
            state.message = L"无法初始化媒体播放器";
            return;
        }
        mfStarted = true;
    }

    StopPlayer(state);

    IMFPMediaPlayer* player = nullptr;
    HRESULT hr = MFPCreateMediaPlayer(state.previewFile.c_str(), FALSE,
                                      MFP_OPTION_NONE, nullptr, state.surface,
                                      &player);
    if (FAILED(hr) || player == nullptr) {
        state.message = L"无法播放该媒体文件";
        return;
    }
    state.player = player;
    state.player->Play();
    state.playing = true;
    state.mediaMode = true;
    state.duration = 0.0;
    state.position = 0.0;
    if (state.hwnd != nullptr) {
        SetTimer(state.hwnd, 1, 200, nullptr);
    }
}

void SetMediaPosition(PreviewState& state, int x) {
    if (state.player == nullptr || state.duration <= 0.0 ||
        state.hwnd == nullptr) {
        return;
    }
    const RECT progress{110, PreviewBottom(state.hwnd) + 16,
                        ClientWidth(state.hwnd) - 20,
                        PreviewBottom(state.hwnd) + 28};
    const int span = progress.right - progress.left;
    if (span <= 0) {
        return;
    }
    double ratio = static_cast<double>(x - progress.left) / span;
    ratio = std::max(0.0, std::min(1.0, ratio));
    PROPVARIANT prop;
    PropVariantInit(&prop);
    prop.vt = VT_I8;
    prop.hVal.QuadPart =
        static_cast<LONGLONG>(state.duration * ratio * 10000000.0);
    state.player->SetPosition(MFP_POSITIONTYPE_100NS, &prop);
    PropVariantClear(&prop);
    state.position = state.duration * ratio;
}

std::wstring FindEntryPath(const PreviewState& state, int entryIndex) {
    for (const ArchiveEntry& entry : state.entries) {
        if (entry.index == entryIndex) {
            return entry.path;
        }
    }
    return std::wstring();
}

std::wstring SourcePath(const PreviewState& state) {
    return state.actualArchivePath.empty() ? state.path : state.actualArchivePath;
}

void StartEntryDrag(HWND hwnd, PreviewState& state, int entryIndex) {
    std::vector<int> indices;
    if (!state.multiSelected.empty() &&
        state.multiSelected.count(entryIndex) > 0) {
        indices.assign(state.multiSelected.begin(), state.multiSelected.end());
    } else {
        indices.push_back(entryIndex);
    }

    std::error_code ec;
    const fs::path tempBase = fs::temp_directory_path(ec);
    if (ec) {
        return;
    }
    const fs::path dragDir = tempBase /
        (L"PopZip_drag_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()));
    fs::create_directories(dragDir, ec);
    if (ec) {
        return;
    }

    std::vector<std::wstring> tempFiles;
    for (int idx : indices) {
        const std::wstring entryPath = FindEntryPath(state, idx);
        const std::wstring fileName = fs::path(entryPath).filename().wstring();
        if (fileName.empty()) {
            continue;
        }
        const fs::path temp = dragDir / fileName;
        std::wstring message;
        if (ExtractArchiveEntry(SourcePath(state), state.password, idx,
                                temp.wstring(), message)) {
            tempFiles.push_back(temp.wstring());
        }
    }

    if (tempFiles.empty()) {
        std::error_code cleanupEc;
        fs::remove_all(dragDir, cleanupEc);
        return;
    }

    IDataObject* data = new CFileDataObject(tempFiles);
    IDropSource* source = new CDropSource;
    DWORD effect = DROPEFFECT_COPY;
    DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
    source->Release();
    data->Release();

    std::error_code removeEc;
    fs::remove_all(dragDir, removeEc);
}

void OpenEntry(HWND hwnd, PreviewState& state, int entryIndex) {
    ArchiveEntry* target = nullptr;
    for (ArchiveEntry& entry : state.entries) {
        if (entry.index == entryIndex) {
            target = &entry;
            break;
        }
    }
    if (target == nullptr) {
        return;
    }
    if (state.loading.load()) {
        return;
    }

    const std::wstring entryName = target->path;
    if (!IsImagePath(entryName) && !IsAudioPath(entryName) &&
        !IsVideoPath(entryName)) {
        state.message = L"该文件类型暂不支持预览";
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    StopPlayer(state);
    DestroyImage(state);
    RemovePreviewFile(state);
    state.message.clear();

    std::error_code ec;
    const fs::path tempBase = fs::temp_directory_path(ec);
    if (ec) {
        state.message = L"无法创建临时文件";
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    const std::wstring extension = fs::path(target->path).extension().wstring();
    const fs::path temp = tempBase /
        (L"PopZip_preview_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()) + extension);
    const std::wstring source = SourcePath(state);
    const std::wstring password = state.password;

    {
        std::lock_guard<std::mutex> lock(g_loadMutex);
        g_loadTemp = temp.wstring();
        g_loadMessage.clear();
        g_loadOk = false;
    }

    state.loading = true;
    state.message = L"正在加载...";
    if (state.surface != nullptr) {
        ShowWindow(state.surface, SW_HIDE);
    }
    SetTimer(hwnd, 2, 200, nullptr);
    InvalidateRect(hwnd, nullptr, FALSE);

    std::thread([hwnd, source, password, entryIndex, temp]() {
        std::wstring message;
        const bool ok = ExtractArchiveEntry(source, password, entryIndex,
                                            temp.wstring(), message);
        {
            std::lock_guard<std::mutex> lock(g_loadMutex);
            g_loadMessage = message;
            g_loadOk = ok;
        }
        PostMessageW(hwnd, WM_APP + 10, ok ? 1 : 0, 0);
    }).detach();
}

void LoadArchive(HWND hwnd, PreviewState& state) {
    state.entries.clear();
    state.display.clear();
    state.currentPrefix.clear();
    state.scroll = 0;
    state.selected = -1;
    state.multiSelected.clear();
    state.message.clear();
    StopPlayer(state);
    DestroyImage(state);
    RemovePreviewFile(state);

    if (state.path.empty()) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (!state.actualArchivePath.empty() &&
        state.actualArchivePath != state.path) {
        ReleaseArchiveCache();
        DeleteFileW(state.actualArchivePath.c_str());
    }
    state.actualArchivePath = state.path;

    const std::wstring path = state.path;

    if (IsImagePath(path)) {
        state.image = LoadImageFile(path, &state.imageWidth, &state.imageHeight);
        state.message = state.image != nullptr ? L"图片预览" : L"无法解码图片";
        InvalidateRect(state.surface, nullptr, FALSE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (IsAudioPath(path) || IsVideoPath(path)) {
        state.previewFile = path;
        state.previewIsTemp = false;
        StartMedia(state);
        if (state.player != nullptr) {
            const std::wstring name = fs::path(path).filename().wstring();
            state.message =
                (IsVideoPath(path) ? L"视频播放中：" : L"音频播放中：") + name;
        }
        InvalidateRect(state.surface, nullptr, FALSE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (!IsPreviewablePath(path)) {
        state.message = L"仅支持压缩包、图片、音频、视频";
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (fs::path(path).extension().wstring() == L".001") {
        {
            std::lock_guard<std::mutex> lock(g_prepareMutex);
            g_prepareActual.clear();
            g_prepareMessage.clear();
            g_prepareOk = false;
        }
        state.loading = true;
        state.message = L"正在合并分卷...";
        if (state.surface != nullptr) {
            ShowWindow(state.surface, SW_HIDE);
        }
        SetTimer(hwnd, 2, 200, nullptr);
        InvalidateRect(hwnd, nullptr, FALSE);

        std::thread([hwnd, path]() {
            std::wstring actual;
            std::wstring message;
            const bool ok = PrepareArchiveSource(path, actual, message);
            {
                std::lock_guard<std::mutex> lock(g_prepareMutex);
                g_prepareActual = actual;
                g_prepareMessage = message;
                g_prepareOk = ok;
            }
            PostMessageW(hwnd, WM_APP + 11, ok ? 1 : 0, 0);
        }).detach();
        return;
    }

    std::wstring prepareMessage;
    if (!PrepareArchiveSource(path, state.actualArchivePath, prepareMessage)) {
        state.message = prepareMessage;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    std::wstring password = state.password;
    if (!ListArchiveEntries(path, password, state.entries, state.message,
                            state.actualArchivePath)) {
        if (password.empty() && state.message == L"需要密码") {
            const std::wstring name = fs::path(path).filename().wstring();
            if (!PromptForPassword(hwnd, name, password)) {
                state.message = L"已取消";
                InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }
            state.password = password;
            if (!ListArchiveEntries(path, password, state.entries,
                                    state.message, state.actualArchivePath)) {
                if (state.message.empty()) {
                    state.message = L"无法读取压缩包内容。";
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }
        } else {
            if (state.message.empty()) {
                state.message = L"无法读取压缩包内容。";
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }

    RefreshDisplay(state);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void ChooseArchive(HWND hwnd, PreviewState& state) {
    std::vector<wchar_t> buffer(4096, L'\0');
    if (!state.path.empty()) {
        const std::wstring file = state.path;
        std::copy(file.begin(), file.end(), buffer.begin());
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    dialog.lpstrFilter =
        L"压缩包\0*.zip;*.zipx;*.7z;*.rar;*.tar;*.gz;*.tgz;*.xz;*.txz;*.iso;*.zpaq;*.001;*.z01\0所有文件\0*.*\0\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    state.path = buffer.data();
    state.password.clear();
    LoadArchive(hwnd, state);
}

void StartArchiveTest(HWND hwnd, PreviewState& state) {
    if (state.path.empty() || state.loading.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_testMutex);
        g_testMessage.clear();
        g_testOk = false;
    }

    state.loading = true;
    state.message = L"正在测试压缩包...";
    if (state.surface != nullptr) {
        ShowWindow(state.surface, SW_HIDE);
    }
    SetTimer(hwnd, 2, 200, nullptr);
    InvalidateRect(hwnd, nullptr, FALSE);

    const std::wstring source = state.path;
    const std::wstring password = state.password;
    const std::wstring actual = state.actualArchivePath;
    std::thread([hwnd, source, password, actual]() {
        std::vector<std::wstring> failed;
        std::wstring message;
        const bool ok = TestArchive(source, password, failed, message, actual);
        std::wstring detail = message;
        if (!failed.empty()) {
            detail += L" 损坏条目：";
            for (size_t i = 0; i < failed.size() && i < 3; ++i) {
                if (i > 0) {
                    detail += L"、";
                }
                detail += failed[i];
            }
            if (failed.size() > 3) {
                detail += L" 等 " + std::to_wstring(failed.size()) + L" 项";
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_testMutex);
            g_testMessage = detail;
            g_testOk = ok;
        }
        PostMessageW(hwnd, WM_APP + 13, ok ? 1 : 0, 0);
    }).detach();
}

LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT message, WPARAM wParam,
                             LPARAM lParam) {
    auto* state = reinterpret_cast<PreviewState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right;
        const int height = client.bottom;

        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
        HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

        if (g_theme >= 2 && g_theme != 3 && g_theme != 5 && state != nullptr) {
            EnsureSky(state->surfaceSky, width, height);
            DrawSky(memory, state->surfaceSky, width, height);
        } else {
            HBRUSH background = CreateSolidBrush(RGB(12, 12, 12));
            FillRect(memory, &client, background);
            DeleteObject(background);
        }

        if (state != nullptr && state->image != nullptr &&
            state->imageWidth > 0 && state->imageHeight > 0) {
            const double scaleX =
                static_cast<double>(width) / state->imageWidth;
            const double scaleY =
                static_cast<double>(height) / state->imageHeight;
            const double scale = std::min(scaleX, scaleY);
            const int drawWidth =
                std::max(1, static_cast<int>(state->imageWidth * scale));
            const int drawHeight =
                std::max(1, static_cast<int>(state->imageHeight * scale));
            const int x = (width - drawWidth) / 2;
            const int y = (height - drawHeight) / 2;

            HDC imageDc = CreateCompatibleDC(memory);
            HGDIOBJ oldImage = SelectObject(imageDc, state->image);
            SetStretchBltMode(memory, HALFTONE);
            StretchBlt(memory, x, y, drawWidth, drawHeight, imageDc, 0, 0,
                       state->imageWidth, state->imageHeight, SRCCOPY);
            SelectObject(imageDc, oldImage);
            DeleteDC(imageDc);
        }

        BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void PaintDialog(HWND hwnd, PreviewState& state) {
    const ThemePalette palette = GetThemePalette(g_theme);
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;

    HDC memoryDc = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

    if (g_theme >= 2) {
        EnsureSky(state.sky, width, height);
        DrawSky(memoryDc, state.sky, width, height);
        const RECT titleBand{0, 0, width, kTitleHeight};
        FillTranslucent(memoryDc, titleBand, palette.background, 205);
    } else {
        HBRUSH background = CreateSolidBrush(palette.background);
        FillRect(memoryDc, &client, background);
        DeleteObject(background);

        HBRUSH titleBrush = CreateSolidBrush(palette.titleBar);
        RECT titleBar{0, 0, width, kTitleHeight};
        FillRect(memoryDc, &titleBar, titleBrush);
        DeleteObject(titleBrush);
    }

    const std::wstring title = state.path.empty()
        ? L"预览压缩包"
        : (L"预览压缩包 - " + state.path);
    DrawTextIn(memoryDc, state.fontTitle, palette.text,
               RECT{20, 0, width - 60, kTitleHeight}, title,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    DrawTextIn(memoryDc, state.fontBody,
               state.hover == 10 ? palette.danger : palette.textSecondary,
               CloseRect(hwnd), L"\u00D7", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    FillRounded(memoryDc, BackRect(), 8,
                state.hover == 11 ? palette.buttonHover : palette.button);
    StrokeRounded(memoryDc, BackRect(), 8, palette.border);
    DrawTextIn(memoryDc, state.fontSmall, palette.text, BackRect(),
               L"返回上级", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (!state.path.empty()) {
        DrawTextIn(memoryDc, state.fontSmall, palette.textSecondary,
                   RECT{132, kToolTop, ClientWidth(hwnd) - 560, kToolBottom},
                   L"\\" + state.currentPrefix,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
                   RECT{132, kToolTop, ClientWidth(hwnd) - 560, kToolBottom},
                   L"未打开压缩包",
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (!state.path.empty()) {
        const RECT searchRect = SearchRect(hwnd);
        FillRounded(memoryDc, searchRect, 8,
                    (state.hover == 19 || state.searchActive)
                        ? palette.panelDeep
                        : palette.button);
        StrokeRounded(memoryDc, searchRect, 8, palette.border);
        const std::wstring searchText =
            state.search.empty() ? L"搜索包内文件…" : state.search;
        DrawTextIn(memoryDc, state.fontSmall,
                   state.search.empty() ? palette.textMuted : palette.text,
                   RECT{searchRect.left + 10, searchRect.top,
                        searchRect.right - 8, searchRect.bottom},
                   searchText,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (state.searchActive) {
            HPEN caretPen = CreatePen(PS_SOLID, 1, palette.text);
            HGDIOBJ oldPen = SelectObject(memoryDc, caretPen);
            const int caretX = searchRect.left + 12;
            MoveToEx(memoryDc, caretX, searchRect.top + 8, nullptr);
            LineTo(memoryDc, caretX, searchRect.bottom - 8);
            SelectObject(memoryDc, oldPen);
            DeleteObject(caretPen);
        }
    }

    if (!state.path.empty()) {
        FillRounded(memoryDc, TestRect(hwnd), 8,
                    state.hover == 18 ? palette.buttonHover : palette.button);
        StrokeRounded(memoryDc, TestRect(hwnd), 8, palette.border);
        DrawTextIn(memoryDc, state.fontSmall, palette.text, TestRect(hwnd),
                   L"测试压缩包", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    FillRounded(memoryDc, OpenRect(hwnd), 8,
                state.hover == 12 ? palette.accentHover : palette.accent);
    DrawTextIn(memoryDc, state.fontSmall, RGB(255, 255, 255), OpenRect(hwnd),
               L"打开压缩包", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const RECT listPanel{kListLeft, kListTop, ListRight(hwnd),
                         PreviewBottom(hwnd)};
    if (g_theme >= 2) {
        FillTranslucentRounded(memoryDc, listPanel, 10, palette.background, 200);
    } else {
        FillRounded(memoryDc, listPanel, 10, palette.panel);
    }
    DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
               RECT{kListLeft + 12, kListTop, 420, kListTop + 24}, L"名称",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
               RECT{430, kListTop, 540, kListTop + 24}, L"大小",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
               RECT{550, kListTop, ListRight(hwnd) - 12, kListTop + 24}, L"类型",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int visibleRows = std::min(
        VisibleRows(hwnd), static_cast<int>(state.display.size()) - state.scroll);
    for (int row = 0; row < visibleRows; ++row) {
        const int index = state.scroll + row;
        const DisplayItem& item = state.display[index];
        RECT rowRect{kListLeft, kListTop + 24 + row * kRowHeight,
                     ListRight(hwnd), kListTop + 24 + (row + 1) * kRowHeight};
        if (index == state.selected ||
            (!item.isDir && state.multiSelected.count(item.entryIndex) > 0)) {
            FillRounded(memoryDc, rowRect, 6, palette.accent);
        } else if (state.hover == 200 + row) {
            FillRounded(memoryDc, rowRect, 6, palette.buttonHover);
        }

        std::wstring displayName = item.isDir ? (L"\U0001F4C1 " + item.name)
                                               : item.name;
        if (item.isDir) {
            displayName = item.name + L"\\";
        }
        DrawTextIn(memoryDc, state.fontSmall, palette.text,
                   RECT{kListLeft + 12, rowRect.top, 420, rowRect.bottom},
                   displayName,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!item.isDir && item.entryIndex >= 0) {
            DrawTextIn(memoryDc, state.fontSmall, palette.textSecondary,
                       RECT{430, rowRect.top, 540, rowRect.bottom},
                       FormatSize(item.size), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
                   RECT{550, rowRect.top, ListRight(hwnd) - 12, rowRect.bottom},
                   item.isDir ? L"文件夹" : L"文件",
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    FillRounded(memoryDc, SurfaceRect(hwnd), 10, RGB(12, 12, 12));

    if (state.loading.load()) {
        FillRounded(memoryDc, SurfaceRect(hwnd), 10, palette.panel);
        DrawTextIn(memoryDc, state.fontBody, palette.textSecondary,
                   SurfaceRect(hwnd), L"正在加载...",
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const RECT surface = SurfaceRect(hwnd);
        RECT bar{surface.left + 60, surface.bottom - 60, surface.right - 60,
                 surface.bottom - 48};
        FillRounded(memoryDc, bar, 6, palette.border);
        const int progress = static_cast<int>((GetTickCount() / 20) % 100);
        RECT filled = bar;
        filled.right = bar.left + (bar.right - bar.left) * progress / 100;
        FillRounded(memoryDc, filled, 6, palette.danger);
    }

    if (state.mediaMode) {
        const int pb = PreviewBottom(hwnd);
        RECT playRect{20, pb + 6, 100, pb + 38};
        FillRounded(memoryDc, playRect, 8,
                    state.hover == 14 ? palette.buttonHover : palette.button);
        StrokeRounded(memoryDc, playRect, 8, palette.border);
        DrawTextIn(memoryDc, state.fontSmall, palette.text, playRect,
                   state.playing ? L"暂停" : L"播放",
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT progressRect{110, pb + 16, width - 20, pb + 28};
        FillRounded(memoryDc, progressRect, 6, palette.panelDeep);
        if (state.duration > 0.0) {
            const double ratio =
                std::max(0.0, std::min(1.0, state.position / state.duration));
            RECT filled = progressRect;
            filled.right = progressRect.left +
                static_cast<int>((progressRect.right - progressRect.left) * ratio);
            FillRounded(memoryDc, filled, 6, palette.accent);
        }
    }

    std::wstring status = state.message;
    if (status.empty()) {
        if (state.path.empty()) {
            status = L"拖入或打开压缩包，双击条目即可预览";
        } else {
            status = L"共 " + std::to_wstring(state.display.size()) + L" 项";
        }
    }
    DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
               RECT{20, height - 40, width - 20, height - 4}, status,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    BitBlt(dc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);
    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    EndPaint(hwnd, &ps);
}

int HitTest(HWND hwnd, const PreviewState& state, int x, int y) {
    const RECT closeRect = CloseRect(hwnd);
    if (x >= closeRect.left && x < closeRect.right && y >= closeRect.top &&
        y < closeRect.bottom) {
        return 10;
    }
    const RECT backRect = BackRect();
    if (x >= backRect.left && x < backRect.right && y >= backRect.top &&
        y < backRect.bottom) {
        return 11;
    }
    const RECT openRect = OpenRect(hwnd);
    if (x >= openRect.left && x < openRect.right && y >= openRect.top &&
        y < openRect.bottom) {
        return 12;
    }
    if (!state.path.empty()) {
        const RECT testRect = TestRect(hwnd);
        if (x >= testRect.left && x < testRect.right && y >= testRect.top &&
            y < testRect.bottom) {
            return 18;
        }
        const RECT searchRect = SearchRect(hwnd);
        if (x >= searchRect.left && x < searchRect.right &&
            y >= searchRect.top && y < searchRect.bottom) {
            return 19;
        }
    }
    if (state.mediaMode) {
        const int pb = PreviewBottom(hwnd);
        if (x >= 20 && x < 100 && y >= pb + 6 && y < pb + 38) {
            return 14;
        }
        if (x >= 110 && x < ClientWidth(hwnd) - 20 && y >= pb + 12 &&
            y < pb + 32) {
            return 15;
        }
    }

    if (x >= kListLeft && x < ListRight(hwnd) && y >= kListTop + 24 &&
        y < kListTop + 24 + VisibleRows(hwnd) * kRowHeight) {
        const int row = (y - kListTop - 24) / kRowHeight;
        if (row >= 0 && row < VisibleRows(hwnd) &&
            state.scroll + row < static_cast<int>(state.display.size())) {
            return 200 + row;
        }
    }
    return 0;
}

void ShowContextMenu(HWND owner, PreviewState* preview,
                     const std::vector<int>& entryIndices, int x, int y);

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    auto* state = reinterpret_cast<PreviewState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            return 0;
        }
        break;

    case WM_NCACTIVATE:
        return TRUE;

    case WM_NCPAINT:
        return 0;

    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<PreviewState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));

        state->fontTitle = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Microsoft YaHei UI");
        state->fontBody = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                      FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH, L"Microsoft YaHei UI");
        state->fontSmall = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Microsoft YaHei UI");

        const RECT surfaceRect = SurfaceRect(hwnd);
        state->surface = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            surfaceRect.left, surfaceRect.top,
            surfaceRect.right - surfaceRect.left,
            surfaceRect.bottom - surfaceRect.top, hwnd, nullptr,
            create->hInstance, nullptr);
        if (state->surface != nullptr) {
            SetWindowLongPtrW(state->surface, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(state));
            SetWindowLongPtrW(state->surface, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(SurfaceProc));
        }
        OleInitialize(nullptr);
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, 3, 100, nullptr);
        return 0;
    }

    case WM_PAINT:
        if (state != nullptr) {
            PaintDialog(hwnd, *state);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        if (state == nullptr) {
            return 0;
        }
        if (!state->trackingMouse) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            state->trackingMouse = true;
        }
        if (state->seeking) {
            SetMediaPosition(*state, GET_X_LPARAM(lParam));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (state->dragIndex >= 0) {
            const int dx = GET_X_LPARAM(lParam) - state->dragStart.x;
            const int dy = GET_Y_LPARAM(lParam) - state->dragStart.y;
            if (dx * dx + dy * dy > 1024) {
                const int entryIndex = state->dragIndex;
                state->dragIndex = -1;
                StartEntryDrag(hwnd, *state, entryIndex);
            }
        }
        const int hover = HitTest(hwnd, *state, GET_X_LPARAM(lParam),
                                  GET_Y_LPARAM(lParam));
        if (hover != state->hover) {
            state->hover = hover;

        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (state != nullptr) {
            state->trackingMouse = false;
            if (state->hover != 0) {
                state->hover = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_MOUSEWHEEL: {
        if (state == nullptr) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &point);
        if (point.x >= kListLeft && point.x < ListRight(hwnd) &&
            point.y >= kListTop && point.y < PreviewBottom(hwnd)) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -2 : 2;
            const int maxScroll = std::max(
                0, static_cast<int>(state->display.size()) - VisibleRows(hwnd));
            state->scroll = std::max(
                0, std::min(maxScroll, state->scroll + delta));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (state == nullptr) {
            return 0;
        }
        const int hit = HitTest(hwnd, *state, GET_X_LPARAM(lParam),
                                GET_Y_LPARAM(lParam));
        if (hit == 15) {
            state->seeking = true;
            SetMediaPosition(*state, GET_X_LPARAM(lParam));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (hit >= 200) {
            const int index = state->scroll + (hit - 200);
            if (index >= 0 &&
                index < static_cast<int>(state->display.size()) &&
                !state->display[index].isDir) {
                state->dragIndex = state->display[index].entryIndex;
                state->dragStart = {GET_X_LPARAM(lParam),
                                    GET_Y_LPARAM(lParam)};
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (state == nullptr) {
            return 0;
        }
        state->dragIndex = -1;
        const int hit = HitTest(hwnd, *state, GET_X_LPARAM(lParam),
                                GET_Y_LPARAM(lParam));
        state->seeking = false;
        if (hit == 10) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (hit == 14) {
            if (state->player != nullptr) {
                if (state->playing) {
                    state->player->Pause();
                    state->playing = false;
                } else {
                    state->player->Play();
                    state->playing = true;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit == 15) {
            SetMediaPosition(*state, GET_X_LPARAM(lParam));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit == 11) {
            state->currentPrefix = ParentPrefix(state->currentPrefix);
            RefreshDisplay(*state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (hit == 12) {
            ChooseArchive(hwnd, *state);
            return 0;
        }
        if (hit == 18) {
            StartArchiveTest(hwnd, *state);
            return 0;
        }
        if (hit == 19) {
            state->searchActive = true;
            SetFocus(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit >= 200) {
            const int index = state->scroll + (hit - 200);
            if (index >= 0 && index < static_cast<int>(state->display.size())) {
                const DisplayItem& item = state->display[index];
                if (!item.isDir && item.entryIndex >= 0) {
                    const bool ctrl =
                        (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (shift && state->selected >= 0) {
                        const int from = std::min(state->selected, index);
                        const int to = std::max(state->selected, index);
                        for (int i = from;
                             i <= to &&
                             i < static_cast<int>(state->display.size());
                             ++i) {
                            if (!state->display[i].isDir) {
                                state->multiSelected.insert(
                                    state->display[i].entryIndex);
                            }
                        }
                        state->selected = index;
                    } else if (ctrl) {
                        if (state->multiSelected.count(item.entryIndex) > 0) {
                            state->multiSelected.erase(item.entryIndex);
                        } else {
                            state->multiSelected.insert(item.entryIndex);
                        }
                        state->selected = index;
                    } else {
                        state->multiSelected.clear();
                        state->multiSelected.insert(item.entryIndex);
                        state->selected = index;
                    }
                } else {
                    state->selected = index;
                    state->multiSelected.clear();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        if (state == nullptr) {
            return 0;
        }
        const int hit = HitTest(hwnd, *state, GET_X_LPARAM(lParam),
                                GET_Y_LPARAM(lParam));
        if (hit >= 200) {
            const int displayIndex = state->scroll + (hit - 200);
            if (displayIndex >= 0 &&
                displayIndex < static_cast<int>(state->display.size())) {
                const DisplayItem& item = state->display[displayIndex];
                if (item.isDir || item.entryIndex < 0) {
                    return 0;
                }
                if (state->multiSelected.count(item.entryIndex) == 0) {
                    state->multiSelected.clear();
                    state->multiSelected.insert(item.entryIndex);
                    state->selected = displayIndex;
                }
                std::vector<int> indices(state->multiSelected.begin(),
                                         state->multiSelected.end());
                POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(hwnd, &screen);
                ShowContextMenu(hwnd, state, indices, screen.x, screen.y);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        if (state == nullptr) {
            return 0;
        }
        state->dragIndex = -1;
        const int hit = HitTest(hwnd, *state, GET_X_LPARAM(lParam),
                                GET_Y_LPARAM(lParam));
        if (hit >= 200) {
            const int index = state->scroll + (hit - 200);
            if (index < 0 || index >= static_cast<int>(state->display.size())) {
                return 0;
            }
            const DisplayItem item = state->display[index];
            if (item.isDir) {
                state->search.clear();
                state->searchActive = false;
                state->currentPrefix += item.name + L"\\";
                RefreshDisplay(*state);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                OpenEntry(hwnd, *state, item.entryIndex);
            }
        }
        return 0;
    }

    case WM_DROPFILES: {
        if (state == nullptr) {
            return 0;
        }
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> dropped;
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0) {
                continue;
            }
            std::wstring item(static_cast<size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, i, item.data(), length + 1);
            item.resize(length);
            if (!item.empty()) {
                dropped.push_back(item);
            }
        }
        DragFinish(drop);

        if (dropped.empty()) {
            return 0;
        }

        if (dropped.size() == 1 && IsPreviewablePath(dropped[0])) {
            state->path = dropped[0];
            state->password.clear();
            LoadArchive(hwnd, *state);
            return 0;
        }

        if (!state->path.empty()) {
            std::wstring message;
            if (AppendFilesToArchive(state->path, state->password, dropped,
                                     message, state->currentPrefix, true)) {
                std::wstring listMessage;
                state->entries.clear();
                ListArchiveEntries(state->path, state->password,
                                   state->entries, listMessage,
                                   state->actualArchivePath);
                RefreshDisplay(*state);
                state->message = L"已添加 " +
                    std::to_wstring(dropped.size()) + L" 个文件";
            } else {
                state->message = message;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_CHAR: {
        if (state == nullptr || !state->searchActive) {
            return 0;
        }
        const wchar_t ch = static_cast<wchar_t>(wParam);
        if (ch == L'\b') {
            if (!state->search.empty()) {
                state->search.pop_back();
            }
        } else if (ch == L'\r' || ch == L'\n') {
            state->searchActive = false;
        } else if (ch == L'\x1B') {
            state->search.clear();
            state->searchActive = false;
        } else if (ch >= 32) {
            state->search.push_back(ch);
        }
        RefreshDisplay(*state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        if (state == nullptr) {
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (state->searchActive) {
                state->search.clear();
                state->searchActive = false;
                RefreshDisplay(*state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        if (wParam == VK_BACK && state->searchActive) {
            if (!state->search.empty()) {
                state->search.pop_back();
                RefreshDisplay(*state);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (wParam == VK_BACK) {
            state->search.clear();
            state->searchActive = false;
            state->currentPrefix = ParentPrefix(state->currentPrefix);
            RefreshDisplay(*state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        const int itemCount = static_cast<int>(state->display.size());
        if (wParam == VK_DOWN || wParam == VK_UP || wParam == VK_HOME ||
            wParam == VK_END) {
            if (itemCount == 0) {
                return 0;
            }
            int index = state->selected;
            if (wParam == VK_DOWN) {
                index = (index < 0) ? 0 : std::min(index + 1, itemCount - 1);
            } else if (wParam == VK_UP) {
                index = (index < 0) ? 0 : std::max(index - 1, 0);
            } else if (wParam == VK_HOME) {
                index = 0;
            } else {
                index = itemCount - 1;
            }
            state->selected = index;
            const int rows = VisibleRows(hwnd);
            if (index < state->scroll) {
                state->scroll = index;
            } else if (index >= state->scroll + rows) {
                state->scroll = index - rows + 1;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_RETURN && state->searchActive) {
            state->searchActive = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_RETURN) {
            const int index = state->selected;
            if (index >= 0 && index < itemCount) {
                const DisplayItem item = state->display[index];
                if (item.isDir) {
                    state->currentPrefix += item.name + L"\\";
                    state->selected = -1;
                    state->scroll = 0;
                    RefreshDisplay(*state);
                } else {
                    OpenEntry(hwnd, *state, item.entryIndex);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 760;
        info->ptMinTrackSize.y = 520;
        return 0;
    }

    case WM_TIMER:
        if (wParam == 3 && state != nullptr && g_theme >= 2) {
            if (state->image != nullptr || state->player != nullptr ||
                state->loading.load()) {
                return 0;
            }
            RECT client{};
            GetClientRect(hwnd, &client);
UpdateSky(state->sky, client.right, client.bottom);
            InvalidateRect(hwnd, nullptr, FALSE);
            if (state->surface != nullptr && g_theme >= 2 && g_theme != 3 &&
                g_theme != 5 && state->image == nullptr &&
                state->player == nullptr) {
                RECT surfaceClient{};
                GetClientRect(state->surface, &surfaceClient);
                UpdateSky(state->surfaceSky, surfaceClient.right,
                          surfaceClient.bottom);
                InvalidateRect(state->surface, nullptr, FALSE);
            }
            return 0;
        }
        if (wParam == 2 && state != nullptr && state->loading.load()) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (state != nullptr && state->player != nullptr) {
            PROPVARIANT prop;
            PropVariantInit(&prop);
            if (state->player->GetPosition(MFP_POSITIONTYPE_100NS, &prop) == S_OK) {
                state->position = prop.hVal.QuadPart / 10000000.0;
            }
            PropVariantClear(&prop);
            PropVariantInit(&prop);
            if (state->player->GetDuration(MFP_POSITIONTYPE_100NS, &prop) == S_OK) {
                state->duration = prop.hVal.QuadPart / 10000000.0;
            }
            PropVariantClear(&prop);
            if (!state->seeking) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_SIZE: {
        if (state != nullptr && state->surface != nullptr) {
            const RECT surfaceRect = SurfaceRect(hwnd);
            SetWindowPos(state->surface, nullptr, surfaceRect.left,
                         surfaceRect.top, surfaceRect.right - surfaceRect.left,
                         surfaceRect.bottom - surfaceRect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &point);
        RECT client{};
        GetClientRect(hwnd, &client);
        const int border = 6;
        const bool left = point.x < border;
        const bool right = point.x >= client.right - border;
        const bool top = point.y < border;
        const bool bottom = point.y >= client.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        if (point.y < kTitleHeight && point.x < ClientWidth(hwnd) - 50) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_APP + 10: {
        if (state == nullptr) {
            return 0;
        }
        std::wstring temp;
        std::wstring message;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(g_loadMutex);
            temp = g_loadTemp;
            message = g_loadMessage;
            ok = g_loadOk;
        }

        state->loading = false;
        KillTimer(hwnd, 2);
        if (state->surface != nullptr) {
            ShowWindow(state->surface, SW_SHOW);
        }

        if (!ok) {
            state->message = message.empty() ? L"加载失败" : message;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        state->previewFile = temp;
        state->previewIsTemp = true;
        if (IsImagePath(temp)) {
            state->image = LoadImageFile(temp, &state->imageWidth,
                                         &state->imageHeight);
            state->message =
                state->image != nullptr ? L"图片预览" : L"无法解码图片";
        } else {
            StartMedia(*state);
            if (state->player != nullptr) {
                state->message = L"播放中";
            }
        }

InvalidateRect(state->surface, nullptr, FALSE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_APP + 11: {
        if (state == nullptr) {
            return 0;
        }
        std::wstring actual;
        std::wstring message;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(g_prepareMutex);
            actual = g_prepareActual;
            message = g_prepareMessage;
            ok = g_prepareOk;
        }

        state->loading = false;
        KillTimer(hwnd, 2);
        if (state->surface != nullptr) {
            ShowWindow(state->surface, SW_SHOW);
        }

        if (!ok) {
            state->message = message.empty() ? L"无法合并分卷。" : message;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        state->actualArchivePath = actual;
        std::wstring password = state->password;
        bool listed = ListArchiveEntries(state->path, password, state->entries,
                                         state->message, actual);
        if (!listed && password.empty() && state->message == L"需要密码") {
            const std::wstring name = fs::path(state->path).filename().wstring();
            if (PromptForPassword(hwnd, name, password)) {
                state->password = password;
                listed = ListArchiveEntries(state->path, password,
                                            state->entries, state->message,
                                            actual);
            } else {
                state->message = L"已取消";
            }
        }
        if (listed) {
            RefreshDisplay(*state);
        } else if (state->message.empty()) {
            state->message = L"无法读取压缩包内容。";
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        if (state->surface != nullptr) {
            InvalidateRect(state->surface, nullptr, FALSE);
        }
        return 0;
    }

    case WM_APP + 13: {
        if (state == nullptr) {
            return 0;
        }
        std::wstring message;
        {
            std::lock_guard<std::mutex> lock(g_testMutex);
            message = g_testMessage;
        }
        state->loading = false;
        KillTimer(hwnd, 2);
        if (state->surface != nullptr) {
            ShowWindow(state->surface, SW_SHOW);
        }
        state->message = message.empty() ? L"测试完成" : message;
        InvalidateRect(hwnd, nullptr, FALSE);
        if (state->surface != nullptr) {
            InvalidateRect(state->surface, nullptr, FALSE);
        }
        return 0;
    }

    case WM_APP + 14: {
        if (state == nullptr) {
            return 0;
        }
        std::wstring message;
        {
            std::lock_guard<std::mutex> lock(g_testMutex);
            message = g_testMessage;
        }
        state->loading = false;
        KillTimer(hwnd, 2);
        if (state->surface != nullptr) {
            ShowWindow(state->surface, SW_SHOW);
        }
        ReleaseArchiveCache();
        std::wstring listMessage;
        state->entries.clear();
        ListArchiveEntries(state->path, state->password, state->entries,
                           listMessage, state->path);
        RefreshDisplay(*state);
        state->message = message.empty() ? L"压缩包已更新" : message;
        InvalidateRect(hwnd, nullptr, FALSE);
        if (state->surface != nullptr) {
            InvalidateRect(state->surface, nullptr, FALSE);
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 3);
        if (state != nullptr) {
            if (!state->actualArchivePath.empty() &&
                state->actualArchivePath != state->path) {
                ReleaseArchiveCache();
                DeleteFileW(state->actualArchivePath.c_str());
            }
            StopPlayer(*state);
            DestroyImage(*state);
            RemovePreviewFile(*state);
            DragAcceptFiles(hwnd, FALSE);
            OleUninitialize();
            if (state->fontTitle != nullptr) DeleteObject(state->fontTitle);
            if (state->fontBody != nullptr) DeleteObject(state->fontBody);
            if (state->fontSmall != nullptr) DeleteObject(state->fontSmall);
            state->fontTitle = nullptr;
            state->fontBody = nullptr;
            state->fontSmall = nullptr;
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return 0;
}

struct ContextMenuState {
    PreviewState* preview = nullptr;
    std::vector<int> entryIndices;
    int hover = 0;
    HWND owner = nullptr;
    HFONT font = nullptr;
};

void CopySelectedPaths(const ContextMenuState& state) {
    std::wstring text;
    for (int idx : state.entryIndices) {
        if (state.preview == nullptr) {
            continue;
        }
        const std::wstring path = FindEntryPath(*state.preview, idx);
        if (path.empty()) {
            continue;
        }
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += path;
    }
    if (text.empty() || !OpenClipboard(state.owner)) {
        return;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem != nullptr) {
        void* dest = GlobalLock(mem);
        if (dest != nullptr) {
            memcpy(dest, text.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
}

void ExtractSelectedToFolder(const ContextMenuState& state) {
    if (state.preview == nullptr || state.entryIndices.empty()) {
        return;
    }
    BROWSEINFOW info{};
    info.hwndOwner = state.owner;
    info.lpszTitle = L"选择解压目录";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&info);
    if (pidl == nullptr) {
        return;
    }
    wchar_t folder[MAX_PATH] = {};
    if (!SHGetPathFromIDListW(pidl, folder)) {
        CoTaskMemFree(pidl);
        return;
    }
    CoTaskMemFree(pidl);

    const fs::path targetRoot(folder);
    std::error_code ec;
    fs::create_directories(targetRoot, ec);
    for (int idx : state.entryIndices) {
        const std::wstring entryPath = FindEntryPath(*state.preview, idx);
        const std::wstring name = fs::path(entryPath).filename().wstring();
        if (name.empty()) {
            continue;
        }
        std::wstring message;
        ExtractArchiveEntry(SourcePath(*state.preview), state.preview->password,
                            idx, (targetRoot / name).wstring(), message);
    }
}

void OpenSelectedFromMenu(const ContextMenuState& state) {
    if (state.preview == nullptr || state.entryIndices.empty()) {
        return;
    }
    PreviewState* preview = state.preview;
    const int entryIndex = state.entryIndices.front();
    const std::wstring path = FindEntryPath(*preview, entryIndex);
    if (path.empty()) {
        return;
    }
    bool isDir = false;
    for (const ArchiveEntry& entry : preview->entries) {
        if (entry.index == entryIndex) {
            isDir = entry.isDir;
            break;
        }
    }
    if (isDir) {
        preview->search.clear();
        preview->searchActive = false;
        preview->currentPrefix = path;
        if (!preview->currentPrefix.empty() &&
            preview->currentPrefix.back() != L'\\') {
            preview->currentPrefix += L'\\';
        }
        preview->selected = -1;
        preview->scroll = 0;
        RefreshDisplay(*preview);
        InvalidateRect(preview->hwnd, nullptr, FALSE);
    } else {
        OpenEntry(preview->hwnd, *preview, entryIndex);
    }
}

void DeleteSelectedFromMenu(const ContextMenuState& state) {
    if (state.preview == nullptr || state.entryIndices.size() != 1) {
        return;
    }
    PreviewState* preview = state.preview;
    if (preview->loading.load()) {
        return;
    }
    if (!preview->actualArchivePath.empty() &&
        preview->actualArchivePath != preview->path) {
        preview->message = L"分卷压缩包不支持删除条目。";
        InvalidateRect(preview->hwnd, nullptr, FALSE);
        return;
    }
    const int entryIndex = state.entryIndices.front();
    const std::wstring entryPath = FindEntryPath(*preview, entryIndex);
    if (entryPath.empty()) {
        return;
    }
    if (!ShowConfirmDialog(state.owner, L"删除条目",
                           L"确定要从压缩包中删除：\n" + entryPath + L"？",
                           L"删除")) {
        return;
    }

    ReleaseArchiveCache();
    preview->loading = true;
    preview->message = L"正在更新压缩包...";
    if (preview->surface != nullptr) {
        ShowWindow(preview->surface, SW_HIDE);
    }
    SetTimer(preview->hwnd, 2, 200, nullptr);
    InvalidateRect(preview->hwnd, nullptr, FALSE);

    const HWND hwnd = preview->hwnd;
    const std::wstring source = preview->path;
    const std::wstring password = preview->password;
    const std::wstring actual = preview->actualArchivePath;
    std::thread([hwnd, source, password, actual, entryPath]() {
        std::vector<std::wstring> remove{entryPath};
        std::wstring message;
        const bool ok =
            RebuildArchiveEntries(source, password, remove, L"", L"", message);
        {
            std::lock_guard<std::mutex> lock(g_testMutex);
            g_testMessage = message;
            g_testOk = ok;
        }
        PostMessageW(hwnd, WM_APP + 14, ok ? 1 : 0, 0);
    }).detach();
}

void RenameSelectedFromMenu(const ContextMenuState& state) {
    if (state.preview == nullptr || state.entryIndices.size() != 1) {
        return;
    }
    PreviewState* preview = state.preview;
    if (preview->loading.load()) {
        return;
    }
    if (!preview->actualArchivePath.empty() &&
        preview->actualArchivePath != preview->path) {
        preview->message = L"分卷压缩包不支持重命名条目。";
        InvalidateRect(preview->hwnd, nullptr, FALSE);
        return;
    }
    const int entryIndex = state.entryIndices.front();
    const std::wstring entryPath = FindEntryPath(*preview, entryIndex);
    if (entryPath.empty()) {
        return;
    }
    const std::wstring oldName = fs::path(entryPath).filename().wstring();
    std::wstring newName;
    if (!ShowRenameDialog(state.owner, oldName, newName)) {
        return;
    }
    if (newName.empty() || newName == oldName) {
        return;
    }
    std::wstring newPath = entryPath;
    const size_t slash = newPath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        newPath = newPath.substr(0, slash + 1) + newName;
    } else {
        newPath = newName;
    }

    ReleaseArchiveCache();
    preview->loading = true;
    preview->message = L"正在更新压缩包...";
    if (preview->surface != nullptr) {
        ShowWindow(preview->surface, SW_HIDE);
    }
    SetTimer(preview->hwnd, 2, 200, nullptr);
    InvalidateRect(preview->hwnd, nullptr, FALSE);

    const HWND hwnd = preview->hwnd;
    const std::wstring source = preview->path;
    const std::wstring password = preview->password;
    std::thread([hwnd, source, password, entryPath, newPath]() {
        std::vector<std::wstring> remove;
        std::wstring message;
        const bool ok =
            RebuildArchiveEntries(source, password, remove, entryPath, newPath,
                                  message);
        {
            std::lock_guard<std::mutex> lock(g_testMutex);
            g_testMessage = message;
            g_testOk = ok;
        }
        PostMessageW(hwnd, WM_APP + 14, ok ? 1 : 0, 0);
    }).detach();
}

LRESULT CALLBACK ContextMenuProc(HWND hwnd, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
    auto* state = reinterpret_cast<ContextMenuState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<ContextMenuState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
        state->font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Microsoft YaHei UI");
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        const ThemePalette palette = GetThemePalette(g_theme);
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH background = CreateSolidBrush(palette.panel);
        FillRect(dc, &client, background);
        DeleteObject(background);

        const wchar_t* items[] = {L"打开", L"删除", L"重命名", L"解压到…",
                                  L"复制路径"};
        for (int i = 0; i < 5; ++i) {
            RECT item{0, 4 + i * 32, client.right, 4 + (i + 1) * 32};
            if (state->hover == i + 1) {
                HBRUSH hoverBrush = CreateSolidBrush(palette.accent);
                FillRect(dc, &item, hoverBrush);
                DeleteObject(hoverBrush);
            }
            SelectObject(dc, state->font);
            SetTextColor(dc, palette.text);
            SetBkMode(dc, TRANSPARENT);
            RECT text{12, item.top, item.right - 8, item.bottom};
            DrawTextW(dc, items[i], -1, &text,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int y = GET_Y_LPARAM(lParam);
        const int hover =
            (y >= 4 && y < 36)
                ? 1
                : ((y >= 36 && y < 68)
                       ? 2
                       : ((y >= 68 && y < 100)
                              ? 3
                              : ((y >= 100 && y < 132)
                                     ? 4
                                     : ((y >= 132 && y < 164) ? 5 : 0))));
        if (state != nullptr && hover != state->hover) {
            state->hover = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const int y = GET_Y_LPARAM(lParam);
        if (state != nullptr && y >= 4 && y < 36) {
            OpenSelectedFromMenu(*state);
        } else if (state != nullptr && y >= 36 && y < 68) {
            DeleteSelectedFromMenu(*state);
        } else if (state != nullptr && y >= 68 && y < 100) {
            RenameSelectedFromMenu(*state);
        } else if (state != nullptr && y >= 100 && y < 132) {
            ExtractSelectedToFolder(*state);
        } else if (state != nullptr && y >= 132 && y < 164) {
            CopySelectedPaths(*state);
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        if (state != nullptr && state->font != nullptr) {
            DeleteObject(state->font);
            state->font = nullptr;
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void ShowContextMenu(HWND owner, PreviewState* preview,
                     const std::vector<int>& entryIndices, int x, int y) {
    static const wchar_t kClassName[] = L"PopZipContextMenu";
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = ContextMenuProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;
        if (!RegisterClassExW(&windowClass)) {
            return;
        }
        registered = true;
    }

    ContextMenuState state;
    state.preview = preview;
    state.entryIndices = entryIndices;
    state.owner = owner;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName,
                                L"", WS_POPUP, x, y, 180, 168, owner, nullptr,
                                instance, &state);
    if (hwnd == nullptr) {
        return;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg{};
    while (IsWindow(hwnd)) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace

bool IsPreviewablePath(const std::wstring& path) {
    return EndsWithLocal(path, L".zip") || EndsWithLocal(path, L".zipx") ||
           EndsWithLocal(path, L".7z") || EndsWithLocal(path, L".rar") ||
           EndsWithLocal(path, L".tar") || EndsWithLocal(path, L".gz") ||
           EndsWithLocal(path, L".tgz") || EndsWithLocal(path, L".xz") ||
           EndsWithLocal(path, L".txz") || EndsWithLocal(path, L".iso") ||
           EndsWithLocal(path, L".zpaq") || EndsWithLocal(path, L".001") ||
           EndsWithLocal(path, L".z01");
}

void OpenPreviewWindow(HWND owner, const std::wstring& path, bool autoPlay,
                       const std::wstring& password) {
    static const wchar_t kClassName[] = L"PopZipArchivePreview";
    static bool classRegistered = false;
    (void)autoPlay;

    HINSTANCE instance = GetModuleHandleW(nullptr);

    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = DialogProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;

        if (!RegisterClassExW(&windowClass)) {
            return;
        }
        classRegistered = true;
    }

    PreviewState state;
    state.path = path;
    state.password = password;

    RECT parentRect{};
    int x = 0;
    int y = 0;
    if (owner != nullptr && GetWindowRect(owner, &parentRect)) {
        x = parentRect.left + (parentRect.right - parentRect.left - kWidth) / 2;
        y = parentRect.top + (parentRect.bottom - parentRect.top - kHeight) / 2;
    } else {
        x = (GetSystemMetrics(SM_CXSCREEN) - kWidth) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - kHeight) / 2;
    }

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"预览压缩包",
                                WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, x,
                                y, kWidth, kHeight, owner, nullptr, instance,
                                &state);
    if (hwnd == nullptr) {
        return;
    }
    state.hwnd = hwnd;

    BOOL darkMode = !IsLightTheme(g_theme);
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode)))) {
        DwmSetWindowAttribute(hwnd, 19, &darkMode, sizeof(darkMode));
    }
    const int cornerPreference = 2;
    DwmSetWindowAttribute(hwnd, 33, &cornerPreference, sizeof(cornerPreference));
    const int nonClientRendering = 2;
    DwmSetWindowAttribute(hwnd, 2, &nonClientRendering, sizeof(nonClientRendering));
    MARGINS margins{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    if (!path.empty()) {
        LoadArchive(hwnd, state);
    }

    MSG message{};
    while (IsWindow(hwnd)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}