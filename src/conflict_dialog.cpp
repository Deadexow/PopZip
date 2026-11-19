#include "conflict_dialog.h"

#include <windowsx.h>
#include <dwmapi.h>

#include "theme.h"

extern int g_theme;

namespace {

constexpr int kWidth = 460;
constexpr int kHeight = 270;
constexpr int kTitleHeight = 46;

struct ConflictState {
    std::wstring dir;
    ConflictChoice choice = ConflictChoice::Cancel;
    int hover = 0;
    bool trackingMouse = false;
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
};

RECT ButtonRect(int index) {
    const int left = 24 + index * 104;
    return RECT{left, 194, left + 100, 230};
}

RECT CloseRect() {
    return RECT{kWidth - 46, 0, kWidth, kTitleHeight};
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
    RECT copy = rc;
    DrawTextW(dc, text.c_str(), -1, &copy, flags);
}

void PaintDialog(HWND hwnd, ConflictState& state) {
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

    HBRUSH background = CreateSolidBrush(palette.background);
    FillRect(memoryDc, &client, background);
    DeleteObject(background);

    HBRUSH titleBrush = CreateSolidBrush(palette.titleBar);
    RECT titleBar{0, 0, width, kTitleHeight};
    FillRect(memoryDc, &titleBar, titleBrush);
    DeleteObject(titleBrush);

    DrawTextIn(memoryDc, state.fontTitle, palette.text,
               RECT{20, 0, width - 60, kTitleHeight}, L"发现同名文件",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(memoryDc, state.fontBody,
               state.hover == 9 ? palette.danger : palette.textSecondary,
               CloseRect(), L"\u00D7",
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(memoryDc, state.fontBody, palette.text,
               RECT{24, 66, width - 24, 96},
               L"目标文件夹中已存在同名文件，请选择处理方式：",
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(memoryDc, state.fontSmall, palette.textMuted,
               RECT{24, 100, width - 24, 148}, state.dir,
               DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

    for (int i = 0; i < 4; ++i) {
        const RECT button = ButtonRect(i);
        const bool primary = (i == 0);
        const bool selected = (i == 0) || (i == 2);
        if (primary) {
            FillRounded(memoryDc, button, 8,
                        state.hover == i + 1 ? palette.accentHover
                                             : palette.accent);
        } else {
            FillRounded(memoryDc, button, 8,
                        state.hover == i + 1 ? palette.buttonHover
                                             : palette.button);
            StrokeRounded(memoryDc, button, 8, palette.border);
        }
        static const wchar_t* labels[] = {L"覆盖", L"跳过", L"保留两者",
                                          L"取消"};
        DrawTextIn(memoryDc, state.fontSmall,
                   selected ? RGB(255, 255, 255) : palette.text, button,
                   labels[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    BitBlt(dc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);
    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    EndPaint(hwnd, &ps);
}

int HitTest(int x, int y) {
    RECT close = CloseRect();
    if (x >= close.left && x < close.right && y >= close.top &&
        y < close.bottom) {
        return 9;
    }
    for (int i = 0; i < 4; ++i) {
        const RECT button = ButtonRect(i);
        if (x >= button.left && x < button.right && y >= button.top &&
            y < button.bottom) {
            return i + 1;
        }
    }
    return 0;
}

void Finish(HWND hwnd, ConflictState& state, ConflictChoice choice) {
    state.choice = choice;
    DestroyWindow(hwnd);
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    auto* state = reinterpret_cast<ConflictState*>(
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
        state = static_cast<ConflictState*>(create->lpCreateParams);
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
        return 0;
    }

    case WM_PAINT:
        if (state != nullptr) {
            PaintDialog(hwnd, *state);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
        if (state != nullptr) {
            if (!state->trackingMouse) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                state->trackingMouse = true;
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
            state->trackingMouse = false;
            if (state->hover != 0) {
                state->hover = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (state != nullptr) {
            const int hit = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hit == 1) {
                Finish(hwnd, *state, ConflictChoice::Overwrite);
            } else if (hit == 2) {
                Finish(hwnd, *state, ConflictChoice::Skip);
            } else if (hit == 3) {
                Finish(hwnd, *state, ConflictChoice::Rename);
            } else if (hit == 4 || hit == 9) {
                Finish(hwnd, *state, ConflictChoice::Cancel);
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (state != nullptr) {
            if (wParam == VK_ESCAPE) {
                Finish(hwnd, *state, ConflictChoice::Cancel);
            } else if (wParam == VK_RETURN) {
                Finish(hwnd, *state, ConflictChoice::Overwrite);
            }
        }
        return 0;

    case WM_CLOSE:
        if (state != nullptr) {
            state->choice = ConflictChoice::Cancel;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
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
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

ConflictChoice ShowConflictDialog(HWND owner, const std::wstring& targetDir) {
    static const wchar_t kClassName[] = L"PopZipConflictWindow";
    static bool classRegistered = false;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DialogProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }

    ConflictState state;
    state.dir = targetDir;

    int x = 0;
    int y = 0;
    if (owner != nullptr) {
        RECT ownerRect{};
        GetWindowRect(owner, &ownerRect);
        x = ownerRect.left + (ownerRect.right - ownerRect.left - kWidth) / 2;
        y = ownerRect.top + (ownerRect.bottom - ownerRect.top - kHeight) / 2;
    }

    HWND hwnd = CreateWindowExW(
        0, kClassName, L"", WS_POPUP | WS_VISIBLE, x, y, kWidth, kHeight,
        owner, nullptr, instance, &state);
    if (hwnd == nullptr) {
        return ConflictChoice::Cancel;
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

    MSG message{};
    while (IsWindow(hwnd) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (owner != nullptr) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return state.choice;
}