#include "settings_dialog.h"

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "compress_engine.h"
#include "eula_dialog.h"
#include "shell_integration.h"
#include "sky.h"
#include "theme.h"

extern bool g_closeAfterExtract;
extern bool g_previewAutoPlay;
extern CompressFormat g_defaultCompressFormat;
extern int g_theme;
extern int g_windowX;
extern int g_windowY;
extern int g_windowWidth;
extern int g_windowHeight;

namespace {

constexpr int kWidth = 460;
constexpr int kHeight = 600;
constexpr int kTitleHeight = 46;

struct SettingsState {
    bool closeAfter = true;
    bool previewAutoPlay = false;
    CompressFormat format = CompressFormat::Zip;
    int theme = 0;
    int themePage = 0;
    int hover = 0;
    bool trackingMouse = false;
    SkyState sky;
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
};

RECT CloseRect() {
    return RECT{kWidth - 46, 0, kWidth, kTitleHeight};
}

RECT CloseAfterRect() {
    return RECT{24, 78, 420, 110};
}

RECT PreviewAutoRect() {
    return RECT{24, 116, 420, 148};
}

RECT FormatButtonRect(int index) {
    const int left = 24 + index * 68;
    return RECT{left, 186, left + 62, 220};
}

RECT ThemeButtonRect(int index) {
    const int col = index % 5;
    const int row = index / 5;
    const int left = 22 + col * 84;
    const int top = 262 + row * 34;
    return RECT{left, top, left + 80, top + 30};
}

RECT PrevPageRect() {
    return RECT{350, 236, 388, 258};
}

RECT NextPageRect() {
    return RECT{396, 236, 434, 258};
}

RECT SaveRect() {
    return RECT{270, 540, 340, 576};
}

RECT EulaRect() {
    return RECT{24, 500, 144, 532};
}

RECT ShellRect() {
    return RECT{160, 500, 316, 532};
}

RECT GitHubRect() {
    return RECT{332, 500, 436, 532};
}

RECT CancelRect() {
    return RECT{356, 540, 436, 576};
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

CompressFormat FormatAt(int index) {
    const CompressFormat order[] = {
        CompressFormat::Zip, CompressFormat::SevenZip, CompressFormat::Tar,
        CompressFormat::Tgz, CompressFormat::TarXz, CompressFormat::Zpaq};
    if (index < 0 || index >= 6) {
        return CompressFormat::Zip;
    }
    return order[index];
}

int FormatIndex(CompressFormat format) {
    for (int i = 0; i < 6; ++i) {
        if (FormatAt(i) == format) {
            return i;
        }
    }
    return 0;
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

void DrawCheckBox(HDC dc, const SettingsState& state, const RECT& row,
                  bool checked, const std::wstring& label, int hover) {
    const ThemePalette palette = GetThemePalette(g_theme);
    RECT box{row.left, row.top + 6, row.left + 20, row.top + 26};
    if (checked) {
        FillRounded(dc, box, 4, palette.accent);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        const int cx = (box.left + box.right) / 2;
        const int cy = (box.top + box.bottom) / 2;
        MoveToEx(dc, cx - 5, cy, nullptr);
        LineTo(dc, cx - 1, cy + 4);
        LineTo(dc, cx + 6, cy - 5);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    } else {
        FillRounded(dc, box, 4, palette.panelDeep);
        StrokeRounded(dc, box, 4, palette.border);
    }

    DrawTextIn(dc, state.fontBody,
               state.hover == hover ? palette.text : palette.textSecondary,
               RECT{row.left + 32, row.top, row.right, row.bottom}, label,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void PaintDialog(HWND hwnd, SettingsState& state) {
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

    DrawTextIn(memoryDc, state.fontTitle, palette.text,
               RECT{20, 0, width - 60, kTitleHeight}, L"设置",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(memoryDc, state.fontBody,
               state.hover == 10 ? palette.danger : palette.textSecondary,
               CloseRect(), L"\u00D7", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DrawCheckBox(memoryDc, state, CloseAfterRect(), state.closeAfter,
                 L"解压后关闭窗口", 11);
    DrawCheckBox(memoryDc, state, PreviewAutoRect(), state.previewAutoPlay,
                 L"预览音视频时自动播放", 12);

    DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
               RECT{24, 162, 420, 182}, L"默认压缩格式",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < 6; ++i) {
        const RECT button = FormatButtonRect(i);
        const bool selected = state.format == FormatAt(i);
        FillRounded(memoryDc, button, 6,
                    selected ? palette.accent
                             : (state.hover == 20 + i ? palette.buttonHover
                                                      : palette.button));
        if (!selected) {
            StrokeRounded(memoryDc, button, 6, palette.border);
        }
        DrawTextIn(memoryDc, state.fontSmall,
                   selected ? RGB(255, 255, 255) : palette.text, button,
                   FormatLabel(FormatAt(i)),
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
               RECT{24, 236, 280, 256}, L"主题",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int themePages = (31 + 24) / 25;
    wchar_t pageText[16] = {};
    wsprintfW(pageText, L"%d/%d", state.themePage + 1, themePages);
    DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
               RECT{280, 236, 340, 256}, pageText,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    FillRounded(memoryDc, PrevPageRect(), 6,
                state.hover == 72 ? palette.buttonHover : palette.button);
    StrokeRounded(memoryDc, PrevPageRect(), 6, palette.border);
    DrawTextIn(memoryDc, state.fontSmall, palette.text, PrevPageRect(),
               L"\u2039", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    FillRounded(memoryDc, NextPageRect(), 6,
                state.hover == 73 ? palette.buttonHover : palette.button);
    StrokeRounded(memoryDc, NextPageRect(), 6, palette.border);
    DrawTextIn(memoryDc, state.fontSmall, palette.text, NextPageRect(),
               L"\u203A", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const wchar_t* themeLabels[] = {
        L"暗夜墨韵", L"皎日白昼", L"星河璀璨", L"深渊黑洞",
        L"樱吹花落", L"蔚蓝地球", L"极光雪原", L"鎏金雨夜",
        L"流萤夏夜", L"落叶秋径", L"深海气泡", L"赛博霓虹",
        L"矩阵代码", L"星云旋涡", L"奶油午后", L"月光白橘",
        L"星空薄荷", L"月球背面", L"电路板纹", L"深海鲸落",
        L"青空物语", L"星塔夜航", L"暮色天桥", L"夏日云路",
        L"夜海孤舟", L"林间夕桥", L"星漠夜穹", L"霓虹都会",
        L"风轮苍穹", L"極", L"曼珠沙华"};
    const int firstTheme = state.themePage * 25;
    const int lastTheme = std::min(31, firstTheme + 25);
    for (int i = firstTheme; i < lastTheme; ++i) {
        const RECT button = ThemeButtonRect(i - firstTheme);
        const bool selected = state.theme == i;
        FillRounded(memoryDc, button, 6,
                    selected ? palette.accent
                             : (state.hover == 40 + i ? palette.buttonHover
                                                      : palette.button));
        if (!selected) {
            StrokeRounded(memoryDc, button, 6, palette.border);
        }
        DrawTextIn(memoryDc, state.fontSmall,
                   selected ? RGB(255, 255, 255) : palette.text, button,
                   themeLabels[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
               RECT{24, 462, kWidth - 24, 486}, L"开发者：ChitoseVulpes",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    FillRounded(memoryDc, EulaRect(), 8,
                state.hover == 60 ? palette.buttonHover : palette.button);
    StrokeRounded(memoryDc, EulaRect(), 8, palette.border);
    DrawTextIn(memoryDc, state.fontSmall, palette.text, EulaRect(),
               L"用户协议", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    FillRounded(memoryDc, ShellRect(), 8,
                state.hover == 61 ? palette.buttonHover : palette.button);
    StrokeRounded(memoryDc, ShellRect(), 8, palette.border);
    DrawTextIn(memoryDc, state.fontSmall, palette.text, ShellRect(),
               IsShellIntegrationEnabled() ? L"取消右键菜单"
                                           : L"注册右键菜单",
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    FillRounded(memoryDc, GitHubRect(), 8,
                state.hover == 62 ? palette.buttonHover : palette.button);
    StrokeRounded(memoryDc, GitHubRect(), 8, palette.border);
    DrawTextIn(memoryDc, state.fontSmall, palette.text, GitHubRect(),
               L"GitHub 主页", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    FillRounded(memoryDc, CancelRect(), 8,
                state.hover == 31 ? palette.buttonHover : palette.button);
    StrokeRounded(memoryDc, CancelRect(), 8, palette.border);
    DrawTextIn(memoryDc, state.fontBody, palette.text, CancelRect(),
               L"取消", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    FillRounded(memoryDc, SaveRect(), 8, palette.accent);
    DrawTextIn(memoryDc, state.fontBody, RGB(255, 255, 255), SaveRect(),
               L"保存", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(dc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);
    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    EndPaint(hwnd, &ps);
}

int HitTest(const SettingsState& state, int x, int y) {
    const RECT closeRect = CloseRect();
    if (x >= closeRect.left && x < closeRect.right && y >= closeRect.top &&
        y < closeRect.bottom) {
        return 10;
    }
    const RECT closeAfter = CloseAfterRect();
    if (x >= closeAfter.left && x < closeAfter.right && y >= closeAfter.top &&
        y < closeAfter.bottom) {
        return 11;
    }
    const RECT preview = PreviewAutoRect();
    if (x >= preview.left && x < preview.right && y >= preview.top &&
        y < preview.bottom) {
        return 12;
    }
    for (int i = 0; i < 6; ++i) {
        const RECT button = FormatButtonRect(i);
        if (x >= button.left && x < button.right && y >= button.top &&
            y < button.bottom) {
            return 20 + i;
        }
    }
    const int firstTheme = state.themePage * 25;
    const int lastTheme = std::min(31, firstTheme + 25);
    for (int i = firstTheme; i < lastTheme; ++i) {
        const RECT button = ThemeButtonRect(i - firstTheme);
        if (x >= button.left && x < button.right && y >= button.top &&
            y < button.bottom) {
            return 40 + i;
        }
    }
    const RECT prevPage = PrevPageRect();
    if (x >= prevPage.left && x < prevPage.right && y >= prevPage.top &&
        y < prevPage.bottom) {
        return 72;
    }
    const RECT nextPage = NextPageRect();
    if (x >= nextPage.left && x < nextPage.right && y >= nextPage.top &&
        y < nextPage.bottom) {
        return 73;
    }
    const RECT cancel = CancelRect();
    if (x >= cancel.left && x < cancel.right && y >= cancel.top &&
        y < cancel.bottom) {
        return 31;
    }
    const RECT save = SaveRect();
    if (x >= save.left && x < save.right && y >= save.top &&
        y < save.bottom) {
        return 32;
    }
    const RECT eula = EulaRect();
    if (x >= eula.left && x < eula.right && y >= eula.top &&
        y < eula.bottom) {
        return 60;
    }
    const RECT shell = ShellRect();
    if (x >= shell.left && x < shell.right && y >= shell.top &&
        y < shell.bottom) {
        return 61;
    }
    const RECT github = GitHubRect();
    if (x >= github.left && x < github.right && y >= github.top &&
        y < github.bottom) {
        return 62;
    }
    (void)state;
    return 0;
}

std::wstring SettingsPath() {
    wchar_t appData[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT,
                     appData);
    const std::wstring dir = std::wstring(appData) + L"\\PopZip";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\settings.ini";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::wstring legacy =
            std::wstring(appData) + L"\\MyUnzip\\settings.ini";
        if (GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES) {
            static const wchar_t* keys[] = {
                L"CloseAfterExtract", L"PreviewAutoPlay", L"DefaultFormat",
                L"Theme", L"WindowX", L"WindowY", L"WindowWidth",
                L"WindowHeight", L"CompressLevel", L"CompressMethod",
                L"CompressThreads", L"CompressVolume"};
            for (const wchar_t* key : keys) {
                wchar_t value[64] = {};
                GetPrivateProfileStringW(L"MyUnzip", key, L"", value, 64,
                                         legacy.c_str());
                if (value[0] != L'\0') {
                    WritePrivateProfileStringW(L"PopZip", key, value,
                                               path.c_str());
                }
            }
        }
    }
    return path;
}

void ApplyState(const SettingsState& state) {
    g_closeAfterExtract = state.closeAfter;
    g_previewAutoPlay = state.previewAutoPlay;
    g_defaultCompressFormat = state.format;
    g_theme = state.theme;

    const std::wstring path = SettingsPath();
    WritePrivateProfileStringW(L"PopZip", L"CloseAfterExtract",
                               state.closeAfter ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"PopZip", L"PreviewAutoPlay",
                               state.previewAutoPlay ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"PopZip", L"DefaultFormat",
                               std::to_wstring(FormatIndex(state.format)).c_str(),
                               path.c_str());
    WritePrivateProfileStringW(L"PopZip", L"Theme",
                               std::to_wstring(state.theme).c_str(),
                               path.c_str());
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsState*>(
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
        state = static_cast<SettingsState*>(create->lpCreateParams);
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

    case WM_LBUTTONUP: {
        if (state == nullptr) {
            return 0;
        }
        const int hit = HitTest(*state, GET_X_LPARAM(lParam),
                                GET_Y_LPARAM(lParam));
        if (hit == 10 || hit == 31) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (hit == 32) {
            ApplyState(*state);
            DestroyWindow(hwnd);
            return 0;
        }
        if (hit == 60) {
            ShowEulaDialog(hwnd);
            return 0;
        }
        if (hit == 61) {
            if (IsShellIntegrationEnabled()) {
                DisableShellIntegration();
            } else {
                EnableShellIntegration();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit == 62) {
            ShellExecuteW(nullptr, L"open",
                          L"https://github.com/Deadexow/PopZip", nullptr,
                          nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (hit == 11) {
            state->closeAfter = !state->closeAfter;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit == 12) {
            state->previewAutoPlay = !state->previewAutoPlay;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit >= 20 && hit < 26) {
            state->format = FormatAt(hit - 20);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit == 72) {
            if (state->themePage > 0) {
                --state->themePage;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (hit == 73) {
            const int pages = (31 + 24) / 25;
            if (state->themePage + 1 < pages) {
                ++state->themePage;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (hit >= 40 && hit < 71) {
            state->theme = hit - 40;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
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

} // namespace

void LoadSettings() {
    const std::wstring path = SettingsPath();
    g_closeAfterExtract =
        GetPrivateProfileIntW(L"PopZip", L"CloseAfterExtract", 1,
                              path.c_str()) != 0;
    g_previewAutoPlay =
        GetPrivateProfileIntW(L"PopZip", L"PreviewAutoPlay", 0,
                              path.c_str()) != 0;
    const int formatIndex =
        GetPrivateProfileIntW(L"PopZip", L"DefaultFormat", 0, path.c_str());
    g_defaultCompressFormat = FormatAt(formatIndex);
    g_theme = GetPrivateProfileIntW(L"PopZip", L"Theme", 0, path.c_str());
    g_windowX =
        GetPrivateProfileIntW(L"PopZip", L"WindowX", -1, path.c_str());
    g_windowY =
        GetPrivateProfileIntW(L"PopZip", L"WindowY", -1, path.c_str());
    g_windowWidth =
        GetPrivateProfileIntW(L"PopZip", L"WindowWidth", 0, path.c_str());
    g_windowHeight =
        GetPrivateProfileIntW(L"PopZip", L"WindowHeight", 0, path.c_str());
}

void SaveMainWindowPlacement(int x, int y, int width, int height) {
    const std::wstring path = SettingsPath();
    WritePrivateProfileStringW(L"PopZip", L"WindowX",
                               std::to_wstring(x).c_str(), path.c_str());
    WritePrivateProfileStringW(L"PopZip", L"WindowY",
                               std::to_wstring(y).c_str(), path.c_str());
    WritePrivateProfileStringW(L"PopZip", L"WindowWidth",
                               std::to_wstring(width).c_str(), path.c_str());
    WritePrivateProfileStringW(L"PopZip", L"WindowHeight",
                               std::to_wstring(height).c_str(), path.c_str());
}

void OpenSettingsWindow(HWND owner) {
    static const wchar_t kClassName[] = L"PopZipSettingsWindow";
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
            return;
        }
        classRegistered = true;
    }

    SettingsState state;
    state.closeAfter = g_closeAfterExtract;
    state.previewAutoPlay = g_previewAutoPlay;
    state.format = g_defaultCompressFormat;
    state.theme = g_theme;
    state.themePage = state.theme / 25;

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

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"设置",
                                WS_POPUP | WS_THICKFRAME, x, y, kWidth, kHeight,
                                owner, nullptr, instance, &state);
    if (hwnd == nullptr) {
        return;
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