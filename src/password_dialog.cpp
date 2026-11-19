#include "password_dialog.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <string>

#include "sky.h"
#include "theme.h"

extern int g_theme;

namespace {

inline ThemePalette Palette() { return GetThemePalette(g_theme); }

constexpr int kIdOk = 1;
constexpr int kIdCancel = 2;

constexpr int kWidth = 460;
constexpr int kHeight = 250;
constexpr int kTitleHeight = 46;

struct DialogState {
    std::wstring archiveName;
    std::wstring password;
    bool accepted = false;
    bool showPassword = false;
    int hoverButton = 0; // 0 无，1 关闭，2 显示密码，3 取消，4 确定
    bool trackingMouse = false;
    SkyState sky;
    HWND edit = nullptr;
    WNDPROC editProc = nullptr;
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
    HBRUSH editBrush = nullptr;
};

RECT CloseRect() {
    return RECT{kWidth - 46, 0, kWidth, kTitleHeight};
}

RECT ShowRect() {
    return RECT{24, 154, 176, 184};
}

RECT CancelRect() {
    return RECT{300, 198, 366, 228};
}

RECT OkRect() {
    return RECT{372, 198, 438, 228};
}

RECT InputCardRect() {
    return RECT{24, 108, 436, 152};
}

RECT InputEditRect() {
    return RECT{36, 116, 424, 142};
}

void FillRounded(HDC dc, const RECT& rc, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HRGN region = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1,
                                     radius, radius);
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

void AcceptDialog(HWND hwnd, DialogState* state) {
    wchar_t buffer[1024]{};
    GetWindowTextW(state->edit, buffer, 1024);
    state->password = buffer;
    state->accepted = true;
    DestroyWindow(hwnd);
}

void CancelDialog(HWND hwnd, DialogState* state) {
    state->accepted = false;
    DestroyWindow(hwnd);
}

void TogglePasswordVisibility(DialogState* state, HWND hwnd) {
    state->showPassword = !state->showPassword;
    SendMessageW(state->edit, EM_SETPASSWORDCHAR,
                 state->showPassword ? 0 : static_cast<WPARAM>(0x25CF), 0);
    InvalidateRect(hwnd, nullptr, FALSE);
    SetFocus(state->edit);
}

void PaintDialog(HWND hwnd, DialogState* state) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;

    HDC memoryDc = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

    HBRUSH background = CreateSolidBrush(Palette().background);
    if (g_theme >= 2) {
        EnsureSky(state->sky, width, height);
        DrawSky(memoryDc, state->sky, width, height);
    } else {
        FillRect(memoryDc, &client, background);
    }
    DeleteObject(background);

    StrokeRounded(memoryDc, RECT{0, 0, width - 1, height - 1}, 10, Palette().border);

    DrawTextIn(memoryDc, state->fontTitle, Palette().text,
               RECT{20, 8, width - 100, 38},
               L"输入密码", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT close = CloseRect();
    if (state->hoverButton == 1) {
        FillRounded(memoryDc, close, 6, RGB(196, 43, 28));
    }
    {
        HPEN pen = CreatePen(PS_SOLID, 1,
                             state->hoverButton == 1 ? RGB(255, 255, 255) : Palette().text);
        HGDIOBJ oldPen = SelectObject(memoryDc, pen);
        const int cx = (close.left + close.right) / 2;
        const int cy = (close.top + close.bottom) / 2;
        MoveToEx(memoryDc, cx - 5, cy - 5, nullptr);
        LineTo(memoryDc, cx + 6, cy + 6);
        MoveToEx(memoryDc, cx - 5, cy + 6, nullptr);
        LineTo(memoryDc, cx + 6, cy - 5);
        SelectObject(memoryDc, oldPen);
        DeleteObject(pen);
    }

    DrawTextIn(memoryDc, state->fontBody, Palette().text,
               RECT{24, 54, 436, 78},
               L"请输入压缩包密码", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const std::wstring archiveName = state->archiveName.empty()
        ? std::wstring(L"压缩包")
        : state->archiveName;
    DrawTextIn(memoryDc, state->fontSmall, Palette().textMuted,
               RECT{24, 80, 436, 100}, archiveName,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT card = InputCardRect();
    FillRounded(memoryDc, card, 8, Palette().panel);
    StrokeRounded(memoryDc, card, 8, Palette().border);

    RECT checkBox{24, 160, 38, 174};
    FillRounded(memoryDc, checkBox, 4,
                state->showPassword ? Palette().accent : Palette().buttonHover);
    if (state->showPassword) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(memoryDc, pen);
        MoveToEx(memoryDc, 28, 167, nullptr);
        LineTo(memoryDc, 31, 171);
        LineTo(memoryDc, 37, 161);
        SelectObject(memoryDc, oldPen);
        DeleteObject(pen);
    }
    DrawTextIn(memoryDc, state->fontSmall, Palette().textSecondary,
               RECT{46, 154, 190, 182}, L"显示密码",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    auto drawButton = [&](const RECT& rect, const wchar_t* text,
                          bool primary, bool hover) {
        COLORREF color = primary
            ? (hover ? Palette().accentHover : Palette().accent)
            : (hover ? Palette().buttonHover : Palette().button);
        FillRounded(memoryDc, rect, 8, color);
        if (!primary) {
            StrokeRounded(memoryDc, rect, 8, Palette().border);
        }
        DrawTextIn(memoryDc, state->fontBody,
                   primary ? RGB(255, 255, 255) : Palette().text,
                   rect, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    drawButton(CancelRect(), L"取消", false, state->hoverButton == 3);
    drawButton(OkRect(), L"确定", true, state->hoverButton == 4);

    DrawTextIn(memoryDc, state->fontSmall, Palette().textMuted,
               RECT{24, 232, 260, 248}, L"Enter 确定  ·  Esc 取消",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    BitBlt(dc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));

    if (message == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            PostMessageW(GetParent(hwnd), WM_COMMAND, kIdOk, 0);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            PostMessageW(GetParent(hwnd), WM_COMMAND, kIdCancel, 0);
            return 0;
        }
    }

    if (state != nullptr && state->editProc != nullptr) {
        return CallWindowProcW(state->editProc, hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<DialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        state->fontTitle = CreateFontW(-16, 0, 0, 0, 600, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                       L"Microsoft YaHei UI");
        state->fontBody = CreateFontW(-15, 0, 0, 0, 400, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                      L"Microsoft YaHei UI");
        state->fontSmall = CreateFontW(-12, 0, 0, 0, 400, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                       L"Microsoft YaHei UI");
        state->editBrush = CreateSolidBrush(Palette().panel);

        const RECT editRect = InputEditRect();
        state->edit = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
            editRect.left, editRect.top,
            editRect.right - editRect.left, editRect.bottom - editRect.top,
            hwnd, reinterpret_cast<HMENU>(100), nullptr, nullptr);

        SendMessageW(state->edit, WM_SETFONT,
                     reinterpret_cast<WPARAM>(state->fontBody), TRUE);
        SendMessageW(state->edit, EM_SETLIMITTEXT, 1024, 0);
        SendMessageW(state->edit, EM_SETPASSWORDCHAR, static_cast<WPARAM>(0x25CF), 0);

        state->editProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(state->edit, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(EditProc)));

        SetFocus(state->edit);
        SetTimer(hwnd, 3, 100, nullptr);
        return 0;
    }

    case WM_PAINT:
        PaintDialog(hwnd, state);
        return 0;

    case WM_ERASEBKGND:
        return 1;

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

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        RECT close = CloseRect();
        if (PtInRect(&close, pt)) {
            return HTCLIENT;
        }
        if (pt.y < kTitleHeight) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_MOUSEMOVE: {
        if (state == nullptr) {
            break;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int hover = 0;
        RECT close = CloseRect();
        RECT show = ShowRect();
        RECT cancel = CancelRect();
        RECT ok = OkRect();

        if (PtInRect(&close, pt)) hover = 1;
        else if (PtInRect(&show, pt)) hover = 2;
        else if (PtInRect(&cancel, pt)) hover = 3;
        else if (PtInRect(&ok, pt)) hover = 4;

        if (hover != state->hoverButton) {
            state->hoverButton = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (!state->trackingMouse) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            state->trackingMouse = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (state != nullptr) {
            state->trackingMouse = false;
            if (state->hoverButton != 0) {
                state->hoverButton = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONUP: {
        if (state == nullptr) {
            break;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT close = CloseRect();
        RECT show = ShowRect();
        RECT cancel = CancelRect();
        RECT ok = OkRect();

        if (PtInRect(&close, pt)) {
            CancelDialog(hwnd, state);
        } else if (PtInRect(&show, pt)) {
            TogglePasswordVisibility(state, hwnd);
        } else if (PtInRect(&cancel, pt)) {
            CancelDialog(hwnd, state);
        } else if (PtInRect(&ok, pt)) {
            AcceptDialog(hwnd, state);
        }
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        if (state != nullptr) {
            HDC editDc = reinterpret_cast<HDC>(wParam);
            SetTextColor(editDc, Palette().text);
            SetBkColor(editDc, Palette().panel);
            return reinterpret_cast<LRESULT>(state->editBrush);
        }
        break;
    }

    case WM_COMMAND:
        if (state == nullptr) {
            break;
        }
        if (LOWORD(wParam) == kIdOk) {
            AcceptDialog(hwnd, state);
        } else if (LOWORD(wParam) == kIdCancel) {
            CancelDialog(hwnd, state);
        }
        return 0;

    case WM_CLOSE:
        if (state != nullptr) {
            CancelDialog(hwnd, state);
        } else {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 3);
        if (state != nullptr) {
            if (state->fontTitle != nullptr) {
                DeleteObject(state->fontTitle);
                state->fontTitle = nullptr;
            }
            if (state->fontBody != nullptr) {
                DeleteObject(state->fontBody);
                state->fontBody = nullptr;
            }
            if (state->fontSmall != nullptr) {
                DeleteObject(state->fontSmall);
                state->fontSmall = nullptr;
            }
            if (state->editBrush != nullptr) {
                DeleteObject(state->editBrush);
                state->editBrush = nullptr;
            }
            state->edit = nullptr;
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

bool PromptForPassword(HWND parent, const std::wstring& archiveName, std::wstring& password) {
    static const wchar_t kClassName[] = L"PopZipPasswordDialog";
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
    state.archiveName = archiveName;

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

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, L"输入密码",
        WS_POPUP | WS_THICKFRAME,
        x, y, kWidth, kHeight,
        parent, nullptr, instance, &state);

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
    if (state.edit != nullptr) {
        SetFocus(state.edit);
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

    if (parent != nullptr) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }

    if (state.accepted) {
        password = state.password;
        return true;
    }

    return false;
}