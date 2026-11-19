#include "compress_dialog.h"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#include "password_dialog.h"
#include "sky.h"
#include "theme.h"

namespace fs = std::filesystem;

extern CompressFormat g_defaultCompressFormat;
extern int g_theme;

namespace {

inline ThemePalette Palette() { return GetThemePalette(g_theme); }

constexpr int kWidth = 780;
constexpr int kHeight = 620;
constexpr int kTitleHeight = 46;
constexpr int kListRows = 8;
constexpr int kRowHeight = 28;
constexpr int kListTop = 88;

struct DialogState {
    std::vector<fs::path> sources;
    fs::path outputPath;
    CompressFormat format = CompressFormat::Zip;
    int level = 5;
    std::wstring password;
    int methodIndex = 0;
    int threadIndex = 0;
    int volumeIndex = 0;
    int selected = 0;
    int scrollOffset = 0;
    int hover = 0;
    bool accepted = false;
    bool trackingMouse = false;
    unsigned long long totalSize = 0;
    int dropdownKind = 0;
    SkyState sky;
    RECT dropdownAnchor{};
    HWND edit = nullptr;
    HBRUSH editBrush = nullptr;
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontButton = nullptr;
};

RECT CloseRect() {
    return RECT{kWidth - 46, 0, kWidth, kTitleHeight};
}

RECT ListRect() {
    return RECT{20, kListTop, 540, kListTop + kListRows * kRowHeight};
}

RECT AddRect() { return RECT{20, 342, 100, 374}; }
RECT DeleteRect() { return RECT{108, 342, 188, 374}; }
RECT OutputRect() { return RECT{120, 382, 430, 410}; }
RECT BrowseRect() { return RECT{438, 382, 540, 410}; }
RECT FormatRect() { return RECT{120, 420, 240, 452}; }
RECT PasswordRect() { return RECT{248, 420, 380, 452}; }
RECT LevelRect() { return RECT{120, 460, 240, 492}; }

RECT MethodRect() { return RECT{560, 108, 760, 140}; }
RECT EncryptRect() { return RECT{560, 166, 760, 198}; }
RECT ThreadRect() { return RECT{560, 224, 760, 256}; }
RECT VolumeRect() { return RECT{560, 296, 760, 328}; }

RECT CancelRect() { return RECT{560, 560, 650, 596}; }
RECT StartRect() { return RECT{660, 560, 760, 596}; }

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

const wchar_t* FormatLabel(CompressFormat format) {
    switch (format) {
    case CompressFormat::Zip:
        return L"ZIP";
    case CompressFormat::SevenZip:
        return L"7Z";
    case CompressFormat::Tar:
        return L"TAR";
    case CompressFormat::Tgz:
        return L"TGZ";
    case CompressFormat::TarXz:
        return L"TAR.XZ";
    case CompressFormat::Zpaq:
        return L"ZPAQ";
    default:
        return L"ZIP";
    }
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

std::vector<const wchar_t*> MethodsForFormat(CompressFormat format) {
    switch (format) {
    case CompressFormat::Zip:
        return {L"Deflate", L"Deflate64", L"BZip2", L"LZMA", L"PPMd"};
    case CompressFormat::SevenZip:
        return {L"LZMA2", L"LZMA", L"BZip2", L"PPMd"};
    case CompressFormat::Zpaq:
        return {L"默认"};
    default:
        return {L"不适用"};
    }
}

std::vector<const wchar_t*> EncryptMethodsForFormat(CompressFormat format) {
    if (format == CompressFormat::Zip) {
        return {L"AES-256"};
    }
    if (format == CompressFormat::SevenZip) {
        return {L"AES-256"};
    }
    return {L"不适用"};
}

std::vector<int> ThreadOptions() {
    return {0, 1, 2, 4, 8};
}

std::wstring ThreadLabel(int threads) {
    if (threads <= 0) {
        return L"自动";
    }
    return std::to_wstring(threads) + L" 线程";
}

void EnsureOutputPath(DialogState& state) {
    if (state.sources.empty()) {
        return;
    }
    if (state.outputPath.empty()) {
        fs::path parent = state.sources[0].parent_path();
        if (parent.empty()) {
            parent = fs::current_path();
        }
        state.outputPath = parent / (state.sources[0].filename().wstring() +
                                      CompressFormatExtension(state.format));
    }
}

void RecalcTotal(DialogState& state) {
    unsigned long long total = 0;
    for (const fs::path& source : state.sources) {
        std::error_code ec;
        if (fs::is_directory(source, ec)) {
            std::error_code iterEc;
            for (fs::recursive_directory_iterator it(source, iterEc), end;
                 !iterEc && it != end; it.increment(iterEc)) {
                std::error_code fileEc;
                if (!it->is_regular_file(fileEc)) {
                    continue;
                }
                std::error_code sizeEc;
                const auto size = it->file_size(sizeEc);
                if (!sizeEc) {
                    total += size;
                }
            }
            continue;
        }
        if (fs::is_regular_file(source, ec)) {
            std::error_code sizeEc;
            const auto size = fs::file_size(source, sizeEc);
            if (!sizeEc) {
                total += size;
            }
        }
    }
    state.totalSize = total;
}

std::wstring CurrentMethod(const DialogState& state) {
    std::vector<const wchar_t*> methods = MethodsForFormat(state.format);
    if (methods.empty()) {
        return L"";
    }
    if (state.methodIndex < 0 ||
        state.methodIndex >= static_cast<int>(methods.size())) {
        return L"";
    }
    const std::wstring value(methods[state.methodIndex]);
    if (value == L"不适用" || value == L"默认") {
        return L"";
    }
    return value;
}

std::wstring PrefsPath() {
    wchar_t appData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::wstring();
    }
    const std::wstring dir = std::wstring(appData) + L"\\PopZip";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

int ReadPref(const wchar_t* key, int fallback) {
    const std::wstring path = PrefsPath();
    if (path.empty()) {
        return fallback;
    }
    return GetPrivateProfileIntW(L"PopZip", key, fallback, path.c_str());
}

void WritePref(const wchar_t* key, int value) {
    const std::wstring path = PrefsPath();
    if (path.empty()) {
        return;
    }
    WritePrivateProfileStringW(L"PopZip", key,
                               std::to_wstring(value).c_str(), path.c_str());
}

unsigned long long VolumeSizeForIndex(int index) {
    static const unsigned long long sizes[] = {
        0ull, 10ull * 1024 * 1024, 50ull * 1024 * 1024,
        100ull * 1024 * 1024, 300ull * 1024 * 1024, 500ull * 1024 * 1024,
        1024ull * 1024 * 1024, 2048ull * 1024 * 1024};
    if (index < 0 || index >= 8) {
        return 0;
    }
    return sizes[index];
}

const wchar_t* VolumeLabel(int index) {
    static const wchar_t* labels[] = {L"不分卷", L"10 MB", L"50 MB",
                                      L"100 MB", L"300 MB", L"500 MB",
                                      L"1 GB", L"2 GB"};
    if (index < 0 || index >= 8) {
        return labels[0];
    }
    return labels[index];
}

std::vector<std::wstring> DropdownItems(const DialogState& state, int kind) {
    std::vector<std::wstring> items;
    switch (kind) {
    case 1:
        items = {L"ZIP", L"7Z", L"TAR", L"TGZ", L"TAR.XZ", L"ZPAQ"};
        break;
    case 2:
        items = {L"快速", L"标准", L"最大"};
        break;
    case 3:
        for (const wchar_t* method : MethodsForFormat(state.format)) {
            items.emplace_back(method);
        }
        break;
    case 4:
        items = {L"自动", L"1 线程", L"2 线程", L"4 线程", L"8 线程"};
        break;
    case 5:
        items = {L"不分卷", L"10 MB", L"50 MB", L"100 MB", L"300 MB",
                 L"500 MB", L"1 GB", L"2 GB"};
        break;
    default:
        break;
    }
    return items;
}

int DropdownSelected(const DialogState& state, int kind) {
    if (kind == 1) {
        const CompressFormat order[] = {
            CompressFormat::Zip, CompressFormat::SevenZip, CompressFormat::Tar,
            CompressFormat::Tgz, CompressFormat::TarXz, CompressFormat::Zpaq};
        for (int i = 0; i < 6; ++i) {
            if (order[i] == state.format) {
                return i;
            }
        }
        return 0;
    }
    if (kind == 2) {
        return state.level == 1 ? 0 : (state.level == 5 ? 1 : 2);
    }
    if (kind == 3) {
        return state.methodIndex;
    }
    if (kind == 4) {
        return state.threadIndex;
    }
    if (kind == 5) {
        return state.volumeIndex;
    }
    return 0;
}

RECT DropdownPanel(const DialogState& state) {
    const int count =
        static_cast<int>(DropdownItems(state, state.dropdownKind).size());
    const int height = count * 30 + 8;
    int top = state.dropdownAnchor.bottom + 4;
    if (top + height > kHeight - 12) {
        top = state.dropdownAnchor.top - 4 - height;
    }
    return RECT{state.dropdownAnchor.left, top, state.dropdownAnchor.right,
                top + height};
}

void ApplyDropdownSelection(DialogState& state, int index) {
    if (state.dropdownKind == 1) {
        const CompressFormat order[] = {
            CompressFormat::Zip, CompressFormat::SevenZip, CompressFormat::Tar,
            CompressFormat::Tgz, CompressFormat::TarXz, CompressFormat::Zpaq};
        if (index >= 0 && index < 6) {
            state.format = order[index];
            state.methodIndex = 0;
            if (!state.outputPath.empty()) {
                state.outputPath.replace_extension(
                    CompressFormatExtension(state.format));
            }
        }
        return;
    }
    if (state.dropdownKind == 2) {
        state.level = index == 0 ? 1 : (index == 1 ? 5 : 9);
        return;
    }
    if (state.dropdownKind == 3) {
        state.methodIndex = index;
        return;
    }
    if (state.dropdownKind == 4) {
        state.threadIndex = index;
        return;
    }
    if (state.dropdownKind == 5) {
        state.volumeIndex = index;
    }
}

int HitTest(const DialogState& state, int x, int y) {
    if (state.dropdownKind != 0) {
        const RECT panel = DropdownPanel(state);
        if (x >= panel.left && x < panel.right && y >= panel.top &&
            y < panel.bottom) {
            const int index = (y - panel.top - 4) / 30;
            const int count = static_cast<int>(
                DropdownItems(state, state.dropdownKind).size());
            if (index >= 0 && index < count) {
                return 300 + index;
            }
        }
        return -1;
    }

    const RECT closeRect = CloseRect();
    if (x >= closeRect.left && x < closeRect.right && y >= closeRect.top &&
        y < closeRect.bottom) {
        return 111;
    }

    const RECT listRect = ListRect();
    if (x >= listRect.left && x < listRect.right && y >= listRect.top &&
        y < listRect.bottom) {
        const int row = (y - kListTop) / kRowHeight;
        const int index = state.scrollOffset + row;
        if (index >= 0 && index < static_cast<int>(state.sources.size())) {
            return 200 + index;
        }
    }

    const RECT addRect = AddRect();
    if (x >= addRect.left && x < addRect.right && y >= addRect.top &&
        y < addRect.bottom) {
        return 105;
    }
    const RECT deleteRect = DeleteRect();
    if (x >= deleteRect.left && x < deleteRect.right && y >= deleteRect.top &&
        y < deleteRect.bottom) {
        return 106;
    }
    const RECT browseRect = BrowseRect();
    if (x >= browseRect.left && x < browseRect.right && y >= browseRect.top &&
        y < browseRect.bottom) {
        return 107;
    }
    const RECT formatRect = FormatRect();
    if (x >= formatRect.left && x < formatRect.right && y >= formatRect.top &&
        y < formatRect.bottom) {
        return 100;
    }
    const RECT passwordRect = PasswordRect();
    if (x >= passwordRect.left && x < passwordRect.right &&
        y >= passwordRect.top && y < passwordRect.bottom) {
        return 108;
    }
    const RECT levelRect = LevelRect();
    if (x >= levelRect.left && x < levelRect.right && y >= levelRect.top &&
        y < levelRect.bottom) {
        return 101;
    }
    const RECT methodRect = MethodRect();
    if (x >= methodRect.left && x < methodRect.right && y >= methodRect.top &&
        y < methodRect.bottom) {
        return 102;
    }
    const RECT threadRect = ThreadRect();
    if (x >= threadRect.left && x < threadRect.right && y >= threadRect.top &&
        y < threadRect.bottom) {
        return 104;
    }
    const RECT volumeRect = VolumeRect();
    if (x >= volumeRect.left && x < volumeRect.right && y >= volumeRect.top &&
        y < volumeRect.bottom) {
        return 112;
    }
    const RECT cancelRect = CancelRect();
    if (x >= cancelRect.left && x < cancelRect.right && y >= cancelRect.top &&
        y < cancelRect.bottom) {
        return 109;
    }
    const RECT startRect = StartRect();
    if (x >= startRect.left && x < startRect.right && y >= startRect.top &&
        y < startRect.bottom) {
        return 110;
    }
    return 0;
}

void DrawDropButton(HDC dc, const DialogState& state, const RECT& rect,
                    const std::wstring& label, bool hover) {
    FillRounded(dc, rect, 8, hover ? Palette().buttonHover : Palette().button);
    StrokeRounded(dc, rect, 8, Palette().border);
    DrawTextIn(dc, state.fontButton, Palette().text, rect, label,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{rect.right - 24, rect.top, rect.right - 8, rect.bottom},
               L"\u25BE", DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void PaintDialog(HWND hwnd, DialogState& state) {
    PAINTSTRUCT ps{};
    HDC windowDc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;

    HDC dc = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    if (g_theme >= 2) {
        EnsureSky(state.sky, width, height);
        DrawSky(dc, state.sky, width, height);
        const RECT titleBand{0, 0, width, kTitleHeight};
        FillTranslucent(dc, titleBand, Palette().background, 205);
    } else {
        HBRUSH background = CreateSolidBrush(Palette().background);
        FillRect(dc, &client, background);
        DeleteObject(background);

        HBRUSH titleBrush = CreateSolidBrush(Palette().titleBar);
        RECT titleBar{0, 0, width, kTitleHeight};
        FillRect(dc, &titleBar, titleBrush);
        DeleteObject(titleBrush);
    }

    DrawTextIn(dc, state.fontTitle, Palette().text,
               RECT{20, 0, width - 60, kTitleHeight}, L"添加文件到压缩包",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(dc, state.fontBody,
               state.hover == 111 ? Palette().danger : Palette().textSecondary,
               CloseRect(), L"\u00D7", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const RECT listRect = ListRect();
    if (g_theme >= 2) {
        FillTranslucentRounded(dc, RECT{20, 64, 540, 310}, 10, Palette().background, 190);
    } else {
        FillRounded(dc, RECT{20, 64, 540, 310}, 10, Palette().panel);
    }

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{30, 64, 250, 86}, L"名称", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{250, 64, 330, 86}, L"大小", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{330, 64, 530, 86}, L"文件路径", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (int row = 0; row < kListRows; ++row) {
        const int index = state.scrollOffset + row;
        if (index >= static_cast<int>(state.sources.size())) {
            break;
        }
        const fs::path& source = state.sources[index];
        RECT rowRect{20, kListTop + row * kRowHeight, 540,
                     kListTop + (row + 1) * kRowHeight};
        if (index == state.selected) {
            FillRounded(dc, rowRect, 6, Palette().accent);
        } else if (state.hover == 200 + index) {
            FillRounded(dc, rowRect, 6, Palette().buttonHover);
        }

        std::error_code ec;
        const bool isDir = fs::is_directory(source, ec);
        std::wstring sizeText = L"文件夹";
        if (!isDir) {
            std::error_code sizeEc;
            const auto size = fs::file_size(source, sizeEc);
            if (!sizeEc) {
                sizeText = FormatSize(size);
            }
        }

        DrawTextIn(dc, state.fontSmall, Palette().text,
                   RECT{30, rowRect.top, 250, rowRect.bottom},
                   source.filename().wstring(),
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextIn(dc, state.fontSmall, Palette().textSecondary,
                   RECT{250, rowRect.top, 330, rowRect.bottom}, sizeText,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(dc, state.fontSmall, Palette().textMuted,
                   RECT{330, rowRect.top, 530, rowRect.bottom},
                   source.parent_path().wstring(),
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{20, 314, 540, 336},
               L"总大小: " + FormatSize(state.totalSize),
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    FillRounded(dc, AddRect(), 8,
                state.hover == 105 ? Palette().buttonHover : Palette().button);
    StrokeRounded(dc, AddRect(), 8, Palette().border);
    DrawTextIn(dc, state.fontButton, Palette().text, AddRect(), L"添加",
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    FillRounded(dc, DeleteRect(), 8,
                state.hover == 106 ? Palette().buttonHover : Palette().button);
    StrokeRounded(dc, DeleteRect(), 8, Palette().border);
    DrawTextIn(dc, state.fontButton, Palette().text, DeleteRect(), L"删除",
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{20, 384, 112, 408}, L"输出文件",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    FillRounded(dc, OutputRect(), 8, Palette().panelDeep);
    StrokeRounded(dc, OutputRect(), 8, Palette().border);

    FillRounded(dc, BrowseRect(), 8,
                state.hover == 107 ? Palette().buttonHover : Palette().button);
    StrokeRounded(dc, BrowseRect(), 8, Palette().border);
    DrawTextIn(dc, state.fontButton, Palette().text, BrowseRect(), L"浏览",
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{20, 424, 112, 448}, L"压缩格式",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawDropButton(dc, state, FormatRect(), FormatLabel(state.format),
                   state.hover == 100);

    std::wstring passwordLabel =
        state.password.empty() ? L"设置密码" : L"密码已设置";
    FillRounded(dc, PasswordRect(), 8,
                state.hover == 108 ? Palette().buttonHover : Palette().button);
    StrokeRounded(dc, PasswordRect(), 8, Palette().border);
    DrawTextIn(dc, state.fontButton,
               state.password.empty() ? Palette().text : Palette().success,
               PasswordRect(), passwordLabel,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{20, 464, 112, 488}, L"压缩级别",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const wchar_t* levelLabel = state.level == 1 ? L"快速"
        : (state.level == 9 ? L"最大" : L"标准");
    DrawDropButton(dc, state, LevelRect(), levelLabel, state.hover == 101);

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{560, 60, 760, 84}, L"高级设置",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{560, 88, 760, 106}, L"压缩方法",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    std::vector<const wchar_t*> methods = MethodsForFormat(state.format);
    std::wstring methodText = methods.empty() ? L"不适用" : methods[
        std::min(state.methodIndex, static_cast<int>(methods.size()) - 1)];
    DrawDropButton(dc, state, MethodRect(), methodText, state.hover == 102);

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{560, 146, 760, 164}, L"加密算法",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    std::vector<const wchar_t*> encrypts = EncryptMethodsForFormat(state.format);
    std::wstring encryptText = encrypts.empty() ? L"不适用" : encrypts[0];
    FillRounded(dc, EncryptRect(), 8, Palette().button);
    StrokeRounded(dc, EncryptRect(), 8, Palette().border);
    DrawTextIn(dc, state.fontButton,
               encrypts[0] == std::wstring(L"不适用") ? Palette().textMuted
                                                      : Palette().text,
               EncryptRect(), encryptText,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{560, 204, 760, 222}, L"CPU 线程数",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    std::vector<int> threads = ThreadOptions();
    const int threadValue = threads[std::min(
        state.threadIndex, static_cast<int>(threads.size()) - 1)];
    DrawDropButton(dc, state, ThreadRect(), ThreadLabel(threadValue),
                   state.hover == 104);

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{560, 276, 760, 294}, L"分卷大小",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawDropButton(dc, state, VolumeRect(), VolumeLabel(state.volumeIndex),
                   state.hover == 112);

    DrawTextIn(dc, state.fontSmall, Palette().textMuted,
               RECT{560, 340, 760, 440},
               L"压缩方法、加密算法、线程数与分卷大小会应用到本次压缩。\n"
               L"TGZ/TAR.XZ 与 ZPAQ 使用默认方法。\n"
               L"分卷按 .001/.002… 切分输出。",
               DT_LEFT | DT_TOP | DT_WORDBREAK);

    FillRounded(dc, CancelRect(), 8,
                state.hover == 109 ? Palette().buttonHover : Palette().button);
    StrokeRounded(dc, CancelRect(), 8, Palette().border);
    DrawTextIn(dc, state.fontButton, Palette().text, CancelRect(), L"取消",
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    FillRounded(dc, StartRect(), 8,
                state.sources.empty() ? Palette().button : Palette().accent);
    DrawTextIn(dc, state.fontButton,
               state.sources.empty() ? Palette().textMuted : RGB(255, 255, 255),
               StartRect(), L"开始压缩",
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (state.dropdownKind != 0) {
        const RECT panel = DropdownPanel(state);
        FillRounded(dc, panel, 8, Palette().panelDeep);
        StrokeRounded(dc, panel, 8, Palette().border);
        const std::vector<std::wstring> items =
            DropdownItems(state, state.dropdownKind);
        const int selected = DropdownSelected(state, state.dropdownKind);
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            RECT itemRect{panel.left + 4, panel.top + 4 + i * 30,
                          panel.right - 4, panel.top + 4 + (i + 1) * 30};
            if (i == selected) {
                FillRounded(dc, itemRect, 6, Palette().accent);
            } else if (state.hover == 300 + i) {
                FillRounded(dc, itemRect, 6, Palette().buttonHover);
            }
            DrawTextIn(dc, state.fontButton, Palette().text, itemRect,
                       items[i], DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    BitBlt(windowDc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

void AddFilesViaDialog(HWND hwnd, DialogState& state) {
    std::vector<wchar_t> buffer(64 * 1024, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    dialog.lpstrFilter = L"所有文件\0*.*\0\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                   OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    std::vector<std::wstring> names;
    const wchar_t* cursor = buffer.data();
    const std::wstring directory(cursor);
    cursor += directory.size() + 1;
    if (*cursor == L'\0') {
        names.push_back(directory);
    } else {
        while (*cursor != L'\0') {
            const std::wstring name(cursor);
            cursor += name.size() + 1;
            names.push_back(directory + L"\\" + name);
        }
    }

    for (const std::wstring& name : names) {
        const fs::path path(name);
        bool exists = false;
        for (const fs::path& source : state.sources) {
            if (source == path) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            state.sources.push_back(path);
        }
    }

    if (state.selected < 0 ||
        state.selected >= static_cast<int>(state.sources.size())) {
        state.selected = 0;
    }
    EnsureOutputPath(state);
    RecalcTotal(state);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void BrowseOutput(HWND hwnd, DialogState& state) {
    std::vector<wchar_t> buffer(4096, L'\0');
    if (!state.outputPath.empty()) {
        const std::wstring file = state.outputPath.wstring();
        std::copy(file.begin(), file.end(), buffer.begin());
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    dialog.lpstrFilter = L"压缩包\0*.zip;*.7z;*.tar;*.tgz;*.tar.xz;*.zpaq\0所有文件\0*.*\0\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"zip";

    if (!GetSaveFileNameW(&dialog)) {
        return;
    }
    state.outputPath = fs::path(buffer.data());
    if (state.edit != nullptr) {
        SetWindowTextW(state.edit, state.outputPath.wstring().c_str());
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void CycleFormat(DialogState& state) {
    const CompressFormat order[] = {
        CompressFormat::Zip, CompressFormat::SevenZip, CompressFormat::Tar,
        CompressFormat::Tgz, CompressFormat::TarXz, CompressFormat::Zpaq};
    int index = 0;
    for (int i = 0; i < 6; ++i) {
        if (order[i] == state.format) {
            index = i;
            break;
        }
    }
    state.format = order[(index + 1) % 6];
    state.methodIndex = 0;
    if (!state.outputPath.empty()) {
        state.outputPath.replace_extension(CompressFormatExtension(state.format));
    }
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    auto* state = reinterpret_cast<DialogState*>(
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

    case WM_TIMER:
        if (state != nullptr && g_theme >= 2) {
            RECT client{};
            GetClientRect(hwnd, &client);
            UpdateSky(state->sky, client.right, client.bottom);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<DialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));

        state->fontTitle = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
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
        state->fontButton = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Microsoft YaHei UI");
        state->editBrush = CreateSolidBrush(Palette().panelDeep);
        state->edit = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            126, 385, 298, 22, hwnd, reinterpret_cast<HMENU>(1),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
            nullptr);
        if (state->edit != nullptr) {
            SendMessageW(state->edit, WM_SETFONT,
                         reinterpret_cast<WPARAM>(state->fontSmall), TRUE);
            SetWindowTextW(state->edit, state->outputPath.wstring().c_str());
            SendMessageW(state->edit, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"选择或输入输出文件"));
        }
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

    case WM_CTLCOLOREDIT:
        if (state != nullptr && state->editBrush != nullptr) {
            HDC editDc = reinterpret_cast<HDC>(wParam);
            SetTextColor(editDc, Palette().text);
            SetBkColor(editDc, Palette().panelDeep);
            return reinterpret_cast<LRESULT>(state->editBrush);
        }
        break;

    case WM_COMMAND:
        if (state != nullptr && LOWORD(wParam) == 1 &&
            HIWORD(wParam) == EN_CHANGE) {
            wchar_t buffer[1024] = {};
            GetWindowTextW(state->edit, buffer, 1024);
            state->outputPath = fs::path(buffer);
        }
        return 0;

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
        const int hover = HitTest(*state, GET_X_LPARAM(lParam),
                                  GET_Y_LPARAM(lParam));
        if (hover != state->hover) {
            state->hover = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
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
        const RECT listRect = ListRect();
        if (point.x >= listRect.left && point.x < listRect.right &&
            point.y >= listRect.top && point.y < listRect.bottom) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1;
            const int maxOffset = std::max(
                0, static_cast<int>(state->sources.size()) - kListRows);
            state->scrollOffset = std::max(
                0, std::min(maxOffset, state->scrollOffset + delta));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (state == nullptr) {
            return 0;
        }
        const int hit = HitTest(*state, GET_X_LPARAM(lParam),
                                GET_Y_LPARAM(lParam));

        if (state->dropdownKind != 0) {
            if (hit >= 300) {
                ApplyDropdownSelection(*state, hit - 300);
                if (state->edit != nullptr) {
                    SetWindowTextW(state->edit,
                                   state->outputPath.wstring().c_str());
                }
            }
            state->dropdownKind = 0;
            if (state->edit != nullptr) {
                ShowWindow(state->edit, SW_SHOW);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (hit == 111 || hit == 109) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (hit == 110) {
            if (state->sources.empty()) {
                return 0;
            }
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (hit == 105) {
            AddFilesViaDialog(hwnd, *state);
            return 0;
        }
        if (hit == 106) {
            if (state->selected >= 0 &&
                state->selected < static_cast<int>(state->sources.size())) {
                state->sources.erase(state->sources.begin() + state->selected);
                if (state->selected >= static_cast<int>(state->sources.size())) {
                    state->selected = static_cast<int>(state->sources.size()) - 1;
                }
                if (state->selected < 0) {
                    state->selected = 0;
                }
                const int maxOffset = std::max(
                    0, static_cast<int>(state->sources.size()) - kListRows);
                state->scrollOffset = std::min(state->scrollOffset, maxOffset);
            }
            RecalcTotal(*state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit == 107) {
            BrowseOutput(hwnd, *state);
            return 0;
        }
        if (hit == 108) {
            std::wstring archiveName =
                state->sources.empty() ? L"压缩包"
                                       : state->sources[0].filename().wstring();
            std::wstring password;
            if (PromptForPassword(hwnd, archiveName, password)) {
                state->password = password;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (hit == 100) {
            state->dropdownKind = 1;
            state->dropdownAnchor = FormatRect();
            if (state->edit != nullptr) ShowWindow(state->edit, SW_HIDE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit == 101) {
            state->dropdownKind = 2;
            state->dropdownAnchor = LevelRect();
            if (state->edit != nullptr) ShowWindow(state->edit, SW_HIDE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit == 102) {
            state->dropdownKind = 3;
            state->dropdownAnchor = MethodRect();
            if (state->edit != nullptr) ShowWindow(state->edit, SW_HIDE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit == 104) {
            state->dropdownKind = 4;
            state->dropdownAnchor = ThreadRect();
            if (state->edit != nullptr) ShowWindow(state->edit, SW_HIDE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit == 112) {
            state->dropdownKind = 5;
            state->dropdownAnchor = VolumeRect();
            if (state->edit != nullptr) ShowWindow(state->edit, SW_HIDE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit >= 200) {
            state->selected = hit - 200;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (state == nullptr) {
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (wParam == VK_RETURN && !state->sources.empty()) {
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_NCHITTEST: {
        const LRESULT base = DefWindowProcW(hwnd, message, wParam, lParam);
        if (base == HTCAPTION || base == HTCLIENT) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            if (point.y < kTitleHeight && point.x < kWidth - 50) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        return base;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 3);
        if (state != nullptr) {
            if (state->fontTitle != nullptr) DeleteObject(state->fontTitle);
            if (state->fontBody != nullptr) DeleteObject(state->fontBody);
            if (state->fontSmall != nullptr) DeleteObject(state->fontSmall);
            if (state->fontButton != nullptr) DeleteObject(state->fontButton);
            if (state->editBrush != nullptr) DeleteObject(state->editBrush);
            state->fontTitle = nullptr;
            state->fontBody = nullptr;
            state->fontSmall = nullptr;
            state->fontButton = nullptr;
            state->editBrush = nullptr;
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return 0;
}

} // namespace

bool ShowCompressDialog(HWND parent, const std::vector<fs::path>& sources,
                        CompressOptions& options) {
    static const wchar_t kClassName[] = L"PopZipCompressDialog";
    static bool classRegistered = false;

    HINSTANCE instance = GetModuleHandleW(nullptr);

    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DialogProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;

        if (!RegisterClassExW(&windowClass)) {
            return false;
        }
        classRegistered = true;
    }

    DialogState state;
    state.format = g_defaultCompressFormat;
    state.level = ReadPref(L"CompressLevel", 5);
    state.methodIndex = ReadPref(L"CompressMethod", 0);
    state.threadIndex = ReadPref(L"CompressThreads", 0);
    state.volumeIndex = ReadPref(L"CompressVolume", 0);
    state.sources = sources;
    if (state.selected >= static_cast<int>(state.sources.size())) {
        state.selected = 0;
    }
    EnsureOutputPath(state);
    RecalcTotal(state);

    RECT parentRect{};
    int x = 0;
    int y = 0;
    if (parent != nullptr && GetWindowRect(parent, &parentRect)) {
        x = parentRect.left + (parentRect.right - parentRect.left - kWidth) / 2;
        y = parentRect.top + (parentRect.bottom - parentRect.top - kHeight) / 2;
    } else {
        x = (GetSystemMetrics(SM_CXSCREEN) - kWidth) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - kHeight) / 2;
    }

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName,
                                L"添加文件到压缩包", WS_POPUP | WS_THICKFRAME,
                                x, y, kWidth, kHeight, parent, nullptr, instance,
                                &state);
    if (hwnd == nullptr) {
        return false;
    }

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

    if (parent != nullptr) {
        EnableWindow(parent, FALSE);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG message{};
    while (IsWindow(hwnd)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (parent != nullptr) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }

    if (!state.accepted) {
        return false;
    }

    WritePref(L"CompressLevel", state.level);
    WritePref(L"CompressMethod", state.methodIndex);
    WritePref(L"CompressThreads", state.threadIndex);
    WritePref(L"CompressVolume", state.volumeIndex);

    EnsureOutputPath(state);

    options.sources = state.sources;
    options.outputFile = state.outputPath;
    options.format = state.format;
    options.level = state.level;
    options.password = state.password;
    options.method = CurrentMethod(state);
    std::vector<int> threads = ThreadOptions();
    options.threads = threads[std::min(
        state.threadIndex, static_cast<int>(threads.size()) - 1)];
    options.volumeSize = VolumeSizeForIndex(state.volumeIndex);
    return true;
}