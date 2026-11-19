#include "eula_dialog.h"

#include <windowsx.h>
#include <dwmapi.h>

#include <string>

#include "theme.h"

extern int g_theme;

namespace {

constexpr int kWidth = 580;
constexpr int kHeight = 470;
constexpr int kTitleHeight = 44;

const wchar_t kEulaText[] =
    L"PopZip 用户协议（最终用户许可协议）\r\n\r\n"
    L"1. 许可\r\n"
    L"   本软件免费提供，可用于个人及商业环境下的压缩与解压。\r\n\r\n"
    L"2. 数据处理\r\n"
    L"   所有压缩包均在本机处理，不收集、不上传任何文件或使用数据；"
    L"输入的密码仅保存在内存中，不写入磁盘。\r\n\r\n"
    L"3. 免责声明\r\n"
    L"   本软件按“现状”提供，开发者不对因使用本软件造成的数据丢失、"
    L"损坏或其它损失承担责任，请在解压或重写压缩包前自行备份重要数据。\r\n\r\n"
    L"4. 第三方组件\r\n"
    L"   内置 7-Zip（www.7-zip.org）与 zpaq（mattmahoney.net）的开源实现，"
    L"遵循其各自许可。\r\n\r\n"
    L"5. 使用限制\r\n"
    L"   请勿将本软件用于任何违法用途。\r\n\r\n"
    L"6. 协议更新\r\n"
    L"   继续使用本软件即表示接受本协议；协议如有变更，将在后续版本中说明。\r\n\r\n"
    L"开发者：ChitoseVulpes\r\n";

struct EulaState {
    HWND edit = nullptr;
    HBRUSH brush = nullptr;
    int hover = 0;
    bool tracking = false;
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
};

RECT EditRect() { return RECT{20, 60, kWidth - 20, 382}; }
RECT CloseButtonRect() { return RECT{kWidth - 120, 400, kWidth - 20, 436}; }
RECT CloseRect() { return RECT{kWidth - 44, 0, kWidth, kTitleHeight}; }

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
    RECT copy = rc;
    DrawTextW(dc, text.c_str(), -1, &copy, flags);
}

void Paint(HWND hwnd, EulaState& state) {
    const ThemePalette palette = GetThemePalette(g_theme);
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);

    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

    HBRUSH background = CreateSolidBrush(palette.background);
    FillRect(memory, &client, background);
    DeleteObject(background);

    HBRUSH titleBrush = CreateSolidBrush(palette.titleBar);
    RECT titleBar{0, 0, client.right, kTitleHeight};
    FillRect(memory, &titleBar, titleBrush);
    DeleteObject(titleBrush);

    DrawTextIn(memory, state.fontTitle, palette.text,
               RECT{20, 0, client.right - 60, kTitleHeight}, L"用户协议",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(memory, state.fontBody,
               state.hover == 9 ? palette.danger : palette.textSecondary,
               CloseRect(), L"\u00D7", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    FillRounded(memory, CloseButtonRect(), 8,
                state.hover == 1 ? palette.accentHover : palette.accent);
    DrawTextIn(memory, state.fontBody, RGB(255, 255, 255), CloseButtonRect(),
               L"关闭", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(hwnd, &ps);
}

int HitTest(int x, int y) {
    const RECT close = CloseRect();
    if (x >= close.left && x < close.right && y >= close.top && y < close.bottom) {
        return 9;
    }
    const RECT button = CloseButtonRect();
    if (x >= button.left && x < button.right && y >= button.top &&
        y < button.bottom) {
        return 1;
    }
    return 0;
}

LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<EulaState*>(
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
    case WM_ERASEBKGND:
        return 1;
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<EulaState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
        state->fontTitle = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Microsoft YaHei UI");
        state->fontBody = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                      FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH, L"Microsoft YaHei UI");

        const RECT editRect = EditRect();
        state->edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", kEulaText,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                ES_AUTOVSCROLL,
            editRect.left, editRect.top, editRect.right - editRect.left,
            editRect.bottom - editRect.top, hwnd,
            reinterpret_cast<HMENU>(1002), GetModuleHandleW(nullptr), nullptr);
        if (state->edit != nullptr) {
            SendMessageW(state->edit, WM_SETFONT,
                         reinterpret_cast<WPARAM>(state->fontBody), TRUE);
        }
        state->brush = CreateSolidBrush(GetThemePalette(g_theme).panelDeep);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (state != nullptr && state->brush != nullptr) {
            const ThemePalette palette = GetThemePalette(g_theme);
            SetTextColor(reinterpret_cast<HDC>(wParam), palette.text);
            SetBkColor(reinterpret_cast<HDC>(wParam), palette.panelDeep);
            return reinterpret_cast<LRESULT>(state->brush);
        }
        break;
    case WM_PAINT:
        if (state != nullptr) {
            Paint(hwnd, *state);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (state != nullptr) {
            if (!state->tracking) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                state->tracking = true;
            }
            const int hover = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hover != state->hover) {
                state->hover = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (state != nullptr) {
            state->tracking = false;
            state->hover = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state != nullptr) {
            const int hit = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hit == 1 || hit == 9) {
                DestroyWindow(hwnd);
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state != nullptr) {
            if (state->brush != nullptr) DeleteObject(state->brush);
            if (state->fontTitle != nullptr) DeleteObject(state->fontTitle);
            if (state->fontBody != nullptr) DeleteObject(state->fontBody);
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

void ShowEulaDialog(HWND owner) {
    static const wchar_t kClassName[] = L"PopZipEulaWindow";
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = Proc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;
        RegisterClassExW(&windowClass);
        registered = true;
    }

    EulaState state;

    int x = 0;
    int y = 0;
    if (owner != nullptr) {
        RECT ownerRect{};
        GetWindowRect(owner, &ownerRect);
        x = ownerRect.left + (ownerRect.right - ownerRect.left - kWidth) / 2;
        y = ownerRect.top + (ownerRect.bottom - ownerRect.top - kHeight) / 2;
    }

    HWND hwnd = CreateWindowExW(0, kClassName, L"", WS_POPUP | WS_VISIBLE, x, y,
                                kWidth, kHeight, owner, nullptr, instance,
                                &state);
    if (hwnd == nullptr) {
        return;
    }

    BOOL darkMode = !IsLightTheme(g_theme);
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode)))) {
        DwmSetWindowAttribute(hwnd, 19, &darkMode, sizeof(darkMode));
    }

    if (owner != nullptr) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetFocus(hwnd);

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (owner != nullptr) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
}