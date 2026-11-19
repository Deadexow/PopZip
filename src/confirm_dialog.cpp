#include "confirm_dialog.h"

#include <windowsx.h>
#include <dwmapi.h>

#include "theme.h"

extern int g_theme;

namespace {

constexpr int kWidth = 440;
constexpr int kHeight = 210;
constexpr int kTitleHeight = 44;

struct ConfirmState {
    std::wstring title;
    std::wstring message;
    std::wstring okLabel;
    bool accepted = false;
    int hover = 0;
    bool tracking = false;
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
};

RECT OkRect() { return RECT{kWidth - 220, kHeight - 56, kWidth - 120, kHeight - 20}; }
RECT CancelRect() { return RECT{kWidth - 110, kHeight - 56, kWidth - 20, kHeight - 20}; }
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

void Paint(HWND hwnd, ConfirmState& state) {
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
               RECT{20, 0, client.right - 60, kTitleHeight}, state.title,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(memory, state.fontBody,
               state.hover == 9 ? palette.danger : palette.textSecondary,
               CloseRect(), L"\u00D7", DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(memory, state.fontBody, palette.text,
               RECT{24, 62, client.right - 24, kHeight - 78}, state.message,
               DT_LEFT | DT_TOP | DT_WORDBREAK);

    FillRounded(memory, OkRect(), 8,
                state.hover == 1 ? palette.danger : palette.accent);
    DrawTextIn(memory, state.fontBody, RGB(255, 255, 255), OkRect(),
               state.okLabel, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    FillRounded(memory, CancelRect(), 8,
                state.hover == 2 ? palette.buttonHover : palette.button);
    StrokeRounded(memory, CancelRect(), 8, palette.border);
    DrawTextIn(memory, state.fontBody, palette.text, CancelRect(), L"取消",
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

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
    const RECT ok = OkRect();
    if (x >= ok.left && x < ok.right && y >= ok.top && y < ok.bottom) {
        return 1;
    }
    const RECT cancel = CancelRect();
    if (x >= cancel.left && x < cancel.right && y >= cancel.top &&
        y < cancel.bottom) {
        return 2;
    }
    return 0;
}

LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ConfirmState*>(
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
        state = static_cast<ConfirmState*>(create->lpCreateParams);
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
        return 0;
    }
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
            if (hit == 1) {
                state->accepted = true;
                DestroyWindow(hwnd);
            } else if (hit == 2 || hit == 9) {
                DestroyWindow(hwnd);
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (state != nullptr) {
            if (wParam == VK_RETURN) {
                state->accepted = true;
                DestroyWindow(hwnd);
            } else if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
            }
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state != nullptr) {
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

bool ShowConfirmDialog(HWND owner, const std::wstring& title,
                       const std::wstring& message,
                       const std::wstring& okLabel) {
    static const wchar_t kClassName[] = L"PopZipConfirmWindow";
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

    ConfirmState state;
    state.title = title;
    state.message = message;
    state.okLabel = okLabel;

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
        return false;
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
    return state.accepted;
}