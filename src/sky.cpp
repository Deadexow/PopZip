#include "sky.h"


#include <algorithm>
#include <wincodec.h>
#include <cmath>
#include <cstdlib>

extern int g_theme;



namespace {

constexpr double kPi = 3.14159265358979323846;

bool g_skySeeded = false;

void SeedSky() {
    if (!g_skySeeded) {
        g_skySeeded = true;
        srand(static_cast<unsigned>(GetTickCount()));
    }
}

COLORREF LerpColor(COLORREF from, COLORREF to, double t) {
    const double r = GetRValue(from) + (GetRValue(to) - GetRValue(from)) * t;
    const double g = GetGValue(from) + (GetGValue(to) - GetGValue(from)) * t;
    const double b = GetBValue(from) + (GetBValue(to) - GetBValue(from)) * t;
    return RGB(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b));
}

void FillFlat(HDC dc, int width, int height, COLORREF color) {
    HBRUSH background = CreateSolidBrush(color);
    RECT rc{0, 0, width, height};
    FillRect(dc, &rc, background);
    DeleteObject(background);
}

void VGradient(HDC dc, int width, int height, COLORREF top, COLORREF bottom) {
    const int bands = 48;
    for (int i = 0; i < bands; ++i) {
        const int y0 = height * i / bands;
        const int y1 = height * (i + 1) / bands + 1;
        const double t = static_cast<double>(i) / (bands - 1);
        HBRUSH brush = CreateSolidBrush(LerpColor(top, bottom, t));
        RECT r{0, y0, width, y1};
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    }
}

void DrawStars(HDC dc, const SkyState& state, int brightBase = 170) {
    for (const SkyStar& star : state.stars) {
        const int brightness = brightBase + star.alpha / 4;
        const COLORREF color =
            RGB(std::min(255, brightness), std::min(255, brightness),
                std::min(255, brightness + 18));
        for (int dx = 0; dx < star.size; ++dx) {
            for (int dy = 0; dy < star.size; ++dy) {
                SetPixel(dc, static_cast<int>(star.x) + dx,
                         static_cast<int>(star.y) + dy, color);
            }
        }
    }
}

void FillSoftCircle(HDC dc, int cx, int cy, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

void StrokeCircle(HDC dc, int cx, int cy, int rx, int ry, COLORREF color,
                  int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - rx, cy - ry, cx + rx, cy + ry);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void FillSoftEllipse(HDC dc, int cx, int cy, int rx, int ry,
                     COLORREF inner, COLORREF outer) {
    for (int i = 5; i >= 1; --i) {
        const int ix = rx * i / 5;
        const int iy = ry * i / 5;
        const COLORREF c =
            LerpColor(inner, outer, static_cast<double>(i - 1) / 4.0);
        HBRUSH b = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(dc, b);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - ix, cy - iy, cx + ix, cy + iy);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(b);
    }
}

void FillRadialGlow(HDC dc, int cx, int cy, int radius, COLORREF color,
                    int peakAlpha) {
    if (radius <= 0 || peakAlpha <= 0) {
        return;
    }
    const int D = 128;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = D;
    bi.bmiHeader.biHeight = -D;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp == nullptr) {
        DeleteDC(mem);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    BYTE* px = static_cast<BYTE*>(bits);
    const double half = D / 2.0;
    const BYTE cr = GetRValue(color);
    const BYTE cg = GetGValue(color);
    const BYTE cb = GetBValue(color);
    for (int y = 0; y < D; ++y) {
        for (int x = 0; x < D; ++x) {
            const double dx = (x + 0.5 - half) / half;
            const double dy = (y + 0.5 - half) / half;
            const double dist = std::sqrt(dx * dx + dy * dy);
            double a = 1.0 - dist;
            if (a < 0.0) {
                a = 0.0;
            }
            a = a * a;
            const BYTE alpha = static_cast<BYTE>(a * peakAlpha);
            const int idx = (y * D + x) * 4;
            px[idx + 0] = static_cast<BYTE>(cb * alpha / 255);
            px[idx + 1] = static_cast<BYTE>(cg * alpha / 255);
            px[idx + 2] = static_cast<BYTE>(cr * alpha / 255);
            px[idx + 3] = alpha;
        }
    }
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(dc, cx - radius, cy - radius, radius * 2, radius * 2, mem, 0, 0,
               D, D, blend);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

HFONT MatrixFont() {
    static HFONT font = nullptr;
    if (font == nullptr) {
        font = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                           FIXED_PITCH, L"Consolas");
    }
    return font;
}

void DrawStarfield(HDC dc, const SkyState& state, int width, int height) {
    FillFlat(dc, width, height, RGB(8, 10, 24));
    DrawStars(dc, state);
    for (const SkyMeteor& meteor : state.meteors) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(220, 230, 255));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, static_cast<int>(meteor.x), static_cast<int>(meteor.y),
                 nullptr);
        LineTo(dc, static_cast<int>(meteor.x - meteor.vx * 12.0f),
               static_cast<int>(meteor.y - meteor.vy * 12.0f));
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
}

void DrawBlackHole(HDC dc, const SkyState& state, int width, int height) {
    FillFlat(dc, width, height, RGB(6, 5, 7));
    DrawStars(dc, state, 130);

    const int cx = width / 2;
    const int cy = height / 2;
    const int r = std::max(40, std::min(width, height) * 30 / 100);

    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));

    for (int i = 3; i >= 0; --i) {
        const double t = static_cast<double>(i) / 3.0;
        const int rx = static_cast<int>(r * (1.34 + 0.16 * t));
        const int ry = static_cast<int>(r * (0.36 + 0.16 * t));
        const COLORREF color =
            LerpColor(RGB(255, 240, 210), RGB(224, 104, 34), t);
        HPEN pen = CreatePen(PS_SOLID, 3, color);
        HGDIOBJ previous = SelectObject(dc, pen);
        Ellipse(dc, cx - rx, cy - static_cast<int>(r * 1.05) - ry,
                cx + rx, cy - static_cast<int>(r * 1.05) + ry);
        Ellipse(dc, cx - rx, cy + static_cast<int>(r * 1.05) - ry,
                cx + rx, cy + static_cast<int>(r * 1.05) + ry);
        SelectObject(dc, previous);
        DeleteObject(pen);
    }

    const int layers = 14;
    for (int i = layers; i >= 0; --i) {
        const double t = static_cast<double>(i) / layers;
        const int rx = static_cast<int>(r * (1.08 + 2.10 * t));
        const int ry = static_cast<int>(r * (0.26 + 0.48 * t));
        if (rx <= 0 || ry <= 0) {
            continue;
        }
        const COLORREF color = LerpColor(RGB(255, 252, 238), RGB(108, 20, 8),
                                         std::pow(t, 0.55));
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ previous = SelectObject(dc, brush);
        Ellipse(dc, cx - rx, cy - ry, cx + rx, cy + ry);
        SelectObject(dc, previous);
        DeleteObject(brush);
    }

    for (int i = 0; i < 170; ++i) {
        const double angle = state.diskAngle + i * (2.0 * kPi / 170.0);
        const double px = cx + std::cos(angle) * r * 2.75;
        const double py = cy + std::sin(angle) * r * 0.60;
        const double front = (std::sin(angle) + 1.0) * 0.5;
        const int brightness = static_cast<int>(175 + 80 * front);
        const COLORREF color =
            RGB(brightness, static_cast<int>(brightness * 0.90),
                static_cast<int>(brightness * 0.66));
        const int dot = front > 0.5 ? 3 : 2;
        for (int dx = 0; dx < dot; ++dx) {
            for (int dy = 0; dy < dot; ++dy) {
                SetPixel(dc, static_cast<int>(px) + dx,
                         static_cast<int>(py) + dy, color);
            }
        }
    }

    HBRUSH horizon = CreateSolidBrush(RGB(0, 0, 0));
    HPEN horizonPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HGDIOBJ previousBrush = SelectObject(dc, horizon);
    HGDIOBJ previousPen = SelectObject(dc, horizonPen);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, previousPen);
    SelectObject(dc, previousBrush);
    DeleteObject(horizonPen);
    DeleteObject(horizon);

    for (int i = 3; i >= 0; --i) {
        const COLORREF color =
            LerpColor(RGB(255, 238, 196), RGB(218, 104, 34), i / 3.0);
        StrokeCircle(dc, cx, cy, r + i, r + i, color, 2);
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

void DrawSakura(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(46, 27, 44), RGB(28, 17, 28));

    HBRUSH brushes[3] = {CreateSolidBrush(RGB(255, 205, 218)),
                         CreateSolidBrush(RGB(250, 174, 197)),
                         CreateSolidBrush(RGB(255, 233, 241))};
    HPEN pens[3] = {CreatePen(PS_SOLID, 1, RGB(212, 126, 158)),
                    CreatePen(PS_SOLID, 1, RGB(206, 112, 148)),
                    CreatePen(PS_SOLID, 1, RGB(222, 148, 176))};

    for (const SkyPetal& petal : state.petals) {
        POINT pts[10];
        const double rx = petal.size;
        const double ry = petal.size * 0.55;
        for (int k = 0; k < 10; ++k) {
            const double angle = petal.angle + k * (2.0 * kPi / 10.0);
            pts[k].x = static_cast<LONG>(petal.x + std::cos(angle) * rx);
            pts[k].y = static_cast<LONG>(petal.y + std::sin(angle) * ry);
        }
        const int shade = std::max(0, std::min(2, petal.shade));
        HGDIOBJ oldBrush = SelectObject(dc, brushes[shade]);
        HGDIOBJ oldPen = SelectObject(dc, pens[shade]);
        Polygon(dc, pts, 10);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
    }
    for (int i = 0; i < 3; ++i) {
        DeleteObject(brushes[i]);
        DeleteObject(pens[i]);
    }
}

void DrawEarth(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(2, 5, 12), RGB(4, 10, 26));
    DrawStars(dc, state, 160);

    const int R = static_cast<int>(std::min(width, height) * 0.42);
    const int cx = static_cast<int>(width * 0.60);
    const int cy = static_cast<int>(height * 0.52);

    FillRadialGlow(dc, cx, cy, R + 34, RGB(70, 140, 220), 90);
    FillSoftCircle(dc, cx, cy, R, RGB(20, 66, 128));

    HRGN earthClip = CreateEllipticRgn(cx - R, cy - R, cx + R, cy + R);
    SelectClipRgn(dc, earthClip);

    for (int i = 5; i >= 0; --i) {
        const int rr = R - i * (R / 6);
        const COLORREF c = LerpColor(RGB(38, 108, 176), RGB(20, 66, 128),
                                     static_cast<double>(i) / 5.0);
        HBRUSH b = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(dc, b);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - rr, cy - rr - R / 8, cx + rr, cy + rr - R / 8);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(b);
    }

    for (const SkyPetal& land : state.petals) {
        if (land.shade == 1) {
            continue;
        }
        const int px = static_cast<int>(land.x);
        const int py = static_cast<int>(land.y);
        const COLORREF base =
            land.size > 22.0f ? RGB(72, 112, 66) : RGB(104, 132, 78);
        for (int i = 4; i >= 1; --i) {
            const int rr = static_cast<int>(land.size * i / 4);
            if (rr <= 0) {
                continue;
            }
            const COLORREF c = LerpColor(base, RGB(20, 66, 128),
                                         (1.0 - i / 4.0) * 0.6);
            HBRUSH b = CreateSolidBrush(c);
            HGDIOBJ ob = SelectObject(dc, b);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, px - rr, py - static_cast<int>(rr * 0.7), px + rr,
                    py + static_cast<int>(rr * 0.7));
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(b);
        }
    }

    for (const SkyPetal& cloud : state.petals) {
        if (cloud.shade == 0) {
            continue;
        }
        const int px = static_cast<int>(cloud.x);
        const int py = static_cast<int>(cloud.y);
        for (int i = 3; i >= 1; --i) {
            const int rr = static_cast<int>(cloud.size * i / 3);
            if (rr <= 0) {
                continue;
            }
            const COLORREF c = LerpColor(RGB(240, 248, 255), RGB(20, 66, 128),
                                         (1.0 - i / 3.0) * 0.5);
            HBRUSH b = CreateSolidBrush(c);
            HGDIOBJ ob = SelectObject(dc, b);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, px - rr, py - static_cast<int>(rr * 0.4), px + rr,
                    py + static_cast<int>(rr * 0.4));
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(b);
        }
    }

    FillRadialGlow(dc, cx - R / 2, cy - R / 2, R, RGB(120, 180, 235), 70);
    FillRadialGlow(dc, cx + R / 2, cy + R / 2, R, RGB(0, 6, 16), 150);

    SelectClipRgn(dc, nullptr);
    DeleteObject(earthClip);

    StrokeCircle(dc, cx, cy, R, R, RGB(140, 205, 255), 2);
}

void DrawAurora(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(5, 14, 22), RGB(7, 9, 26));
    DrawStars(dc, state, 120);

    const double now = state.time;

    auto drawCurtain = [&](double centerX, double widthRatio,
                           double heightRatio, double phase, COLORREF topColor,
                           COLORREF botColor, int peakAlpha) {
        const int srcW = 32;
        const int srcH = 64;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = srcW;
        bi.bmiHeader.biHeight = -srcH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp =
            CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp == nullptr) {
            DeleteDC(mem);
            return;
        }
        HGDIOBJ old = SelectObject(mem, bmp);
        BYTE* px = static_cast<BYTE*>(bits);
        for (int y = 0; y < srcH; ++y) {
            const double ty = static_cast<double>(y) / (srcH - 1);
            double va = std::pow(ty, 1.15) * (1.0 - 0.08 * (1.0 - ty));
            if (va < 0.0) va = 0.0;
            const COLORREF col = LerpColor(topColor, botColor, ty);
            for (int x = 0; x < srcW; ++x) {
                const double hx =
                    std::fabs(x - (srcW - 1) / 2.0) / ((srcW - 1) / 2.0);
                double ha = 1.0 - hx * hx;
                if (ha < 0.0) ha = 0.0;
                const double a = va * ha;
                const BYTE alpha = static_cast<BYTE>(
                    std::max(0.0, std::min(1.0, a)) * peakAlpha);
                const int idx = (y * srcW + x) * 4;
                px[idx + 0] = static_cast<BYTE>(GetBValue(col) * alpha / 255);
                px[idx + 1] = static_cast<BYTE>(GetGValue(col) * alpha / 255);
                px[idx + 2] = static_cast<BYTE>(GetRValue(col) * alpha / 255);
                px[idx + 3] = alpha;
            }
        }
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        const int baseline = static_cast<int>(height * 0.86);
        const double cx = centerX * width + std::sin(now * 0.5 + phase) * 22.0;
        const double hh = height * heightRatio *
                          (0.88 + 0.10 * std::sin(now * 0.6 + phase) +
                           0.04 * std::sin(now * 1.3 + phase * 2.0));
        const int curH = static_cast<int>(hh);
        const int curW = static_cast<int>(width * widthRatio);
        if (curH > 2 && curW > 2) {
            AlphaBlend(dc, static_cast<int>(cx - curW / 2.0), baseline - curH,
                       curW, curH, mem, 0, 0, srcW, srcH, blend);
        }
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    };

    drawCurtain(0.14, 0.34, 0.62, 0.0, RGB(50, 90, 150), RGB(90, 245, 165), 150);
    drawCurtain(0.34, 0.30, 0.78, 1.5, RGB(70, 80, 165), RGB(110, 235, 180), 165);
    drawCurtain(0.52, 0.28, 0.66, 3.0, RGB(60, 70, 155), RGB(130, 220, 200), 140);
    drawCurtain(0.70, 0.32, 0.80, 4.4, RGB(80, 70, 160), RGB(100, 240, 175), 155);
    drawCurtain(0.88, 0.30, 0.60, 5.7, RGB(55, 85, 150), RGB(95, 225, 190), 130);

    const int rayBase = static_cast<int>(height * 0.84);
    for (int i = 0; i < 46; ++i) {
        const int sx = (i * 47 + 13) % std::max(1, width) +
                       static_cast<int>(std::sin(now * 0.3 + i) * 10.0);
        const double lenR = 0.18 + 0.28 * (((i * 53) % 100) / 100.0);
        const int len = static_cast<int>(height * lenR);
        const double hue = 0.5 + 0.5 * std::sin(i * 1.7 + now * 0.2);
        for (int yy = 0; yy < len; yy += 2) {
            const double a = 1.0 - static_cast<double>(yy) / len;
            const int r = static_cast<int>(70 * a);
            const int g = static_cast<int>((170 + 70 * hue) * a);
            const int b = static_cast<int>((120 + 80 * hue) * a);
            SetPixel(dc, sx, rayBase - yy, RGB(r, g, b));
        }
    }

    for (const SkyPetal& snow : state.petals) {
        const int px = static_cast<int>(snow.x);
        const int py = static_cast<int>(snow.y);
        SetPixel(dc, px, py, RGB(210, 225, 240));
    }
}

void DrawRain(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(26, 20, 13), RGB(14, 11, 8));

    for (int i = 0; i < 4; ++i) {
        const int cy = height - 90 + i * 24;
        const double pulse = 0.5 + 0.5 * std::sin(state.time * 1.3 + i);
        const int alpha = static_cast<int>(40 + 50 * pulse);
        const COLORREF color = RGB(alpha + 60, alpha + 30, 10);
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ ob = SelectObject(dc, brush);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, width / 2 - 240, cy - 10, width / 2 + 240, cy + 10);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(brush);
    }

    for (const SkyPetal& rain : state.petals) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(206, 178, 110));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        const int x = static_cast<int>(rain.x);
        const int y = static_cast<int>(rain.y);
        MoveToEx(dc, x, y, nullptr);
        LineTo(dc, x + 6, y + static_cast<int>(rain.size));
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
}

void DrawFireflies(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(8, 22, 16), RGB(4, 12, 9));

    for (const SkyPetal& bug : state.petals) {
        const double twinkle =
            0.5 + 0.5 * std::sin(state.time * 2.2 + bug.phase);
        const int fx = static_cast<int>(bug.x +
                                        std::sin(state.time * 0.8 + bug.phase) *
                                            14.0);
        const int fy = static_cast<int>(bug.y +
                                        std::cos(state.time * 0.6 + bug.phase) *
                                            18.0);
        const int bright = static_cast<int>(90 + 165 * twinkle);
        const int halo = static_cast<int>(70 * twinkle);
        if (halo > 6) {
            FillSoftCircle(dc, fx, fy, static_cast<int>(bug.size) + 4,
                           RGB(halo / 2, halo, halo / 4));
        }
        const int core = static_cast<int>(bug.size);
        FillSoftCircle(dc, fx, fy, core,
                       RGB(bright, bright, static_cast<int>(bright * 0.55)));
    }
}

void DrawMaple(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(58, 32, 16), RGB(26, 15, 9));

    for (int i = 0; i < 5; ++i) {
        const int gx = static_cast<int>(width * (0.15 + 0.16 * i) +
                                        std::sin(state.time * 0.4 + i) * 30);
        FillSoftCircle(dc, gx, static_cast<int>(height * 0.2), 40,
                       RGB(70, 42, 18));
    }

    for (const SkyPetal& leaf : state.petals) {
        POINT pts[5];
        const double rx = leaf.size;
        for (int k = 0; k < 5; ++k) {
            const double angle = leaf.angle + k * (2.0 * kPi / 5.0);
            const double radius = (k % 2 == 0) ? rx : rx * 0.5;
            pts[k].x = static_cast<LONG>(leaf.x + std::cos(angle) * radius);
            pts[k].y = static_cast<LONG>(leaf.y + std::sin(angle) * radius);
        }
        COLORREF color = leaf.shade == 0 ? RGB(232, 148, 46)
                        : leaf.shade == 1 ? RGB(208, 96, 30)
                                          : RGB(240, 186, 76);
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(150, 70, 20));
        HGDIOBJ ob = SelectObject(dc, brush);
        HGDIOBJ op = SelectObject(dc, pen);
        Polygon(dc, pts, 5);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(brush);
        DeleteObject(pen);
    }
}

void DrawBubbles(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(8, 26, 50), RGB(5, 14, 32));

    for (int i = 0; i < 3; ++i) {
        const int gx = static_cast<int>(width * (0.2 + 0.3 * i) +
                                        std::sin(state.time * 0.5 + i) * 40);
        FillSoftCircle(dc, gx, static_cast<int>(height * 0.85), 60,
                       RGB(16, 44, 78));
    }

    for (const SkyPetal& bubble : state.petals) {
        const int bx = static_cast<int>(bubble.x);
        const int by = static_cast<int>(bubble.y);
        const int rr = static_cast<int>(bubble.size);
        StrokeCircle(dc, bx, by, rr, rr, RGB(120, 190, 240), 1);
        SetPixel(dc, bx - rr / 3, by - rr / 3, RGB(220, 245, 255));
        SetPixel(dc, bx - rr / 3 + 1, by - rr / 3, RGB(200, 230, 250));
    }
}

void DrawCyber(HDC dc, const SkyState& state, int width, int height) {
    FillFlat(dc, width, height, RGB(14, 8, 24));

    const double pulse = 0.5 + 0.5 * std::sin(state.time * 1.6);
    const int gridBright = static_cast<int>(30 + 50 * pulse);
    HPEN gridPen = CreatePen(PS_SOLID, 1,
                             RGB(gridBright, static_cast<int>(gridBright * 0.4),
                                 static_cast<int>(gridBright * 1.4)));
    HGDIOBJ oldPen = SelectObject(dc, gridPen);
    for (int x = 0; x <= width; x += 48) {
        MoveToEx(dc, x, 0, nullptr);
        LineTo(dc, x, height);
    }
    for (int y = 0; y <= height; y += 48) {
        MoveToEx(dc, 0, y, nullptr);
        LineTo(dc, width, y);
    }
    SelectObject(dc, oldPen);
    DeleteObject(gridPen);

    for (const SkyPetal& particle : state.petals) {
        const int px = static_cast<int>(particle.x);
        const int py = static_cast<int>(particle.y);
        const COLORREF color = particle.shade == 0 ? RGB(255, 80, 210)
                                                   : RGB(90, 230, 255);
        for (int k = 0; k < static_cast<int>(particle.size); ++k) {
            SetPixel(dc, px - k * 3, py, LerpColor(color, RGB(14, 8, 24),
                                                   static_cast<double>(k) /
                                                       particle.size));
        }
    }
}

void DrawMatrix(HDC dc, const SkyState& state, int width, int height) {
    FillFlat(dc, width, height, RGB(2, 6, 2));

    HFONT font = MatrixFont();
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    const wchar_t* glyphs =
        L"0123456789ABCDEFアイウエオカキクケコサシスセソタチツテトナニヌネノ";
    const int glen = 48;
    const int cell = 15;

    for (const SkyPetal& column : state.petals) {
        const int x = static_cast<int>(column.x);
        const int headY = static_cast<int>(column.y);
        const int count = static_cast<int>(column.size);
        const int seed = static_cast<int>(column.phase);
        for (int k = 0; k < count; ++k) {
            const int yy = headY - k * cell;
            if (yy < -cell || yy > height) {
                continue;
            }
            int idx = (seed * 7 + k * 13 + 3) % glen;
            if (idx < 0) {
                idx += glen;
            }
            const wchar_t ch[2] = {glyphs[idx], 0};
            int green = 255 - k * (210 / std::max(1, count));
            if (green < 40) {
                green = 40;
            }
            const COLORREF color = k == 0
                ? RGB(200, 255, 210)
                : RGB(static_cast<int>(green * 0.45), green,
                      static_cast<int>(green * 0.45));
            SetTextColor(dc, color);
            TextOutW(dc, x, yy, ch, 1);
        }
    }
    SelectObject(dc, oldFont);
}

void DrawNebula(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(12, 9, 32), RGB(6, 6, 18));
    DrawStars(dc, state, 150);

    const int cx = width / 2;
    const int cy = height / 2;
    const double t = state.time;
    const int maxR = static_cast<int>(std::min(width, height) * 0.46);

    FillRadialGlow(dc, cx, cy, maxR, RGB(146, 108, 216), 235);
    FillRadialGlow(dc, cx, cy, maxR * 2 / 3, RGB(198, 150, 244), 150);
    FillRadialGlow(dc, cx, cy, 64, RGB(236, 214, 255), 180);

    for (int i = 0; i < 760; ++i) {
        const double d = static_cast<double>(i) / 760.0;
        const double radius = 12.0 + std::pow(d, 0.85) * maxR * 1.15;
        const int arm = (i % 2 == 0) ? 0 : 1;
        const double angle = arm * kPi + d * 5.2 + t * 0.20;
        const int px = cx + static_cast<int>(std::cos(angle) * radius);
        const int py = cy + static_cast<int>(std::sin(angle) * radius * 0.74);
        COLORREF color = LerpColor(RGB(238, 206, 255), RGB(92, 72, 172), d);
        if ((i % 9) == 0) {
            color = RGB(255, 236, 255);
        }
        SetPixel(dc, px, py, color);
        if (d < 0.35) {
            SetPixel(dc, px + 1, py, color);
        }
    }

    for (int arm = 0; arm < 2; ++arm) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(108, 82, 178));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        bool first = true;
        for (int s = 0; s <= 170; ++s) {
            const double d = static_cast<double>(s) / 170.0;
            const double radius = 14.0 + d * maxR * 1.15;
            const double angle = arm * kPi + d * 5.2 + t * 0.20;
            const int px = cx + static_cast<int>(std::cos(angle) * radius);
            const int py = cy + static_cast<int>(std::sin(angle) * radius * 0.74);
            if (first) {
                MoveToEx(dc, px, py, nullptr);
                first = false;
            } else {
                LineTo(dc, px, py);
            }
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    FillRadialGlow(dc, cx, cy, 26, RGB(250, 244, 255), 220);
}

void DrawMoonBack(HDC dc, const SkyState& state, int width, int height) {
    FillFlat(dc, width, height, RGB(6, 6, 10));
    DrawStars(dc, state, 120);

    const int R = static_cast<int>(std::min(width, height) * 0.40);
    const int cx = width / 2;
    const int cy = height / 2;

    FillRadialGlow(dc, cx, cy, R + 30, RGB(120, 130, 150), 55);
    FillSoftCircle(dc, cx, cy, R, RGB(146, 148, 154));

    HRGN clip = CreateEllipticRgn(cx - R, cy - R, cx + R, cy + R);
    SelectClipRgn(dc, clip);

    for (const SkyPetal& crater : state.petals) {
        const int px = static_cast<int>(crater.x);
        const int py = static_cast<int>(crater.y);
        const int rr = static_cast<int>(crater.size);
        if (rr <= 0) {
            continue;
        }
        if (crater.shade == 0) {
            FillRadialGlow(dc, px, py, rr, RGB(64, 64, 74), 130);
        } else {
            FillRadialGlow(dc, px, py, rr, RGB(40, 40, 48), 140);
            StrokeCircle(dc, px, py, rr, rr, RGB(196, 198, 206), 1);
        }
    }

    FillRadialGlow(dc, cx - R / 2, cy - R / 2, R, RGB(220, 222, 228), 90);
    FillRadialGlow(dc, cx + R / 2, cy + R / 2, R, RGB(6, 6, 12), 150);

    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    StrokeCircle(dc, cx, cy, R, R, RGB(70, 72, 82), 2);
}

void DrawCircuit(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(6, 26, 16), RGB(4, 18, 12));

    const int step = 54;
    HPEN dim = CreatePen(PS_SOLID, 1, RGB(26, 66, 42));
    HGDIOBJ oldPen = SelectObject(dc, dim);
    for (int y = step / 2; y < height; y += step) {
        MoveToEx(dc, 0, y, nullptr);
        int yy = y;
        for (int x = step; x <= width; x += step) {
            yy = y + ((x / step) % 3 == 1 ? step / 2 : 0) -
                 ((x / step) % 6 == 3 ? step / 2 : 0);
            LineTo(dc, x, yy);
        }
    }
    for (int x = step / 2; x < width; x += step) {
        MoveToEx(dc, x, 0, nullptr);
        int xx = x;
        for (int y = step; y <= height; y += step) {
            xx = x + ((y / step) % 4 == 2 ? step / 2 : 0);
            LineTo(dc, xx, y);
        }
    }
    SelectObject(dc, oldPen);
    DeleteObject(dim);

    for (int y = step / 2; y < height; y += step) {
        for (int x = step / 2; x < width; x += step) {
            StrokeCircle(dc, x, y, 3, 3, RGB(190, 158, 74), 1);
            FillSoftCircle(dc, x, y, 1, RGB(228, 196, 110));
        }
    }

    const double t = state.time;
    for (int k = 0; k < 4; ++k) {
        const int y = step / 2 + (k * 2 + 1) * step;
        if (y >= height) {
            continue;
        }
        const int px = static_cast<int>(std::fmod(t * 90.0 + k * 190.0,
                                                  static_cast<double>(width)));
        FillSoftCircle(dc, px, y, 4, RGB(90, 230, 130));
        FillSoftCircle(dc, px, y, 2, RGB(210, 255, 220));
    }
}

void DrawWhale(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(10, 30, 50), RGB(3, 8, 16));

    for (int i = 0; i < 4; ++i) {
        const int bx = static_cast<int>(width * (0.16 + 0.2 * i) +
                                        std::sin(state.time * 0.4 + i) * 18);
        for (int w = 0; w < 3; ++w) {
            HPEN pen = CreatePen(PS_SOLID, 20 - w * 6,
                                 RGB(14 + w * 6, 38 + w * 8, 56 + w * 10));
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, bx + w * 10, 0, nullptr);
            LineTo(dc, bx + 70 + w * 10, height);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
    }

    for (const SkyPetal& snow : state.petals) {
        SetPixel(dc, static_cast<int>(snow.x), static_cast<int>(snow.y),
                 RGB(180, 205, 225));
        if (snow.size >= 2.0f) {
            SetPixel(dc, static_cast<int>(snow.x) + 1,
                     static_cast<int>(snow.y), RGB(150, 178, 200));
        }
    }

    const int hx = static_cast<int>(width * 0.16);
    const int hy = static_cast<int>(height * 0.60);
    const int tx = static_cast<int>(width * 0.88);
    const int ty = static_cast<int>(height * 0.72);

    auto spineY = [&](double xr) {
        return hy + (ty - hy) * xr - static_cast<int>(height * 0.05) *
                                          std::sin(xr * kPi);
    };

    HPEN spine = CreatePen(PS_SOLID, 3, RGB(178, 188, 198));
    HGDIOBJ oldPen = SelectObject(dc, spine);
    bool first = true;
    for (double xr = 0.0; xr <= 1.0001; xr += 0.02) {
        const int px = hx + static_cast<int>((tx - hx) * xr);
        const int py = spineY(xr);
        if (first) {
            MoveToEx(dc, px, py, nullptr);
            first = false;
        } else {
            LineTo(dc, px, py);
        }
    }
    SelectObject(dc, oldPen);
    DeleteObject(spine);

    HPEN rib = CreatePen(PS_SOLID, 2, RGB(150, 162, 176));
    oldPen = SelectObject(dc, rib);
    for (double xr = 0.18; xr <= 0.72; xr += 0.06) {
        const int px = hx + static_cast<int>((tx - hx) * xr);
        const int py = spineY(xr);
        const int len = static_cast<int>(height * (0.10 + 0.12 * std::sin(xr * 3.1)));
        MoveToEx(dc, px, py, nullptr);
        LineTo(dc, px - len / 3, py + len);
    }
    SelectObject(dc, oldPen);
    DeleteObject(rib);

    StrokeCircle(dc, hx, static_cast<int>(height * 0.56),
                 static_cast<int>(width * 0.05),
                 static_cast<int>(height * 0.06), RGB(170, 182, 192), 2);
    StrokeCircle(dc, tx, ty, static_cast<int>(width * 0.03),
                 static_cast<int>(height * 0.02), RGB(160, 172, 184), 2);
}

void DrawCream(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(252, 246, 234), RGB(244, 226, 200));

    for (const SkyPetal& blob : state.petals) {
        const int bx = static_cast<int>(blob.x +
                                        std::sin(state.time * 0.5 + blob.phase) *
                                            22.0);
        const int by = static_cast<int>(blob.y +
                                        std::cos(state.time * 0.4 + blob.phase) *
                                            16.0);
        const int radius = static_cast<int>(blob.size);
        for (int i = 5; i >= 1; --i) {
            const int rr = radius * i / 5;
            const COLORREF color = LerpColor(RGB(255, 250, 238),
                                             RGB(246, 226, 196),
                                             static_cast<double>(i) / 5.0);
            FillSoftCircle(dc, bx, by, rr, color);
        }
    }
}

void DrawMoon(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(253, 250, 245), RGB(247, 239, 227));

    const int mx = static_cast<int>(width * 0.80);
    const int my = static_cast<int>(height * 0.22);
    const double breath = 0.5 + 0.5 * std::sin(state.time * 0.9);
    for (int i = 18; i >= 1; --i) {
        const int rr = 28 + i * 6;
        const double tt = static_cast<double>(i) / 18.0;
        const COLORREF color = LerpColor(RGB(255, 253, 246), RGB(247, 239, 227),
                                         tt);
        FillSoftCircle(dc, mx, my, rr, color);
    }
    FillSoftCircle(dc, mx, my, 26,
                   RGB(255, 253, static_cast<int>(244 + 4 * breath)));

    for (int i = 9; i >= 1; --i) {
        const int rr = 14 + i * 5;
        const double tt = static_cast<double>(i) / 9.0;
        const COLORREF color = LerpColor(RGB(254, 251, 244), RGB(247, 239, 227),
                                         tt);
        FillSoftCircle(dc, static_cast<int>(width * 0.16),
                       static_cast<int>(height * 0.18), rr, color);
    }

    for (const SkyPetal& cloud : state.petals) {
        const int cx = static_cast<int>(cloud.x);
        const int cy = static_cast<int>(cloud.y);
        const int rx = static_cast<int>(cloud.size);
        const int ry = static_cast<int>(cloud.size * 0.30);
        for (int i = 3; i >= 1; --i) {
            const int ix = rx * i / 3;
            const int iy = ry * i / 3;
            const COLORREF c = LerpColor(RGB(251, 246, 237), RGB(247, 239, 227),
                                         static_cast<double>(i) / 3.0);
            HBRUSH brush = CreateSolidBrush(c);
            HGDIOBJ ob = SelectObject(dc, brush);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, cx - ix, cy - iy, cx + ix, cy + iy);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(brush);
        }
    }
}

void DrawMint(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(14, 28, 30), RGB(10, 20, 22));

    for (const SkyStar& star : state.stars) {
        const double twinkle =
            0.4 + 0.6 * std::sin(state.time * 1.8 + star.x * 0.1 + star.y * 0.1);
        const int brightness = static_cast<int>(160 + 90 * twinkle);
        SetPixel(dc, static_cast<int>(star.x), static_cast<int>(star.y),
                 RGB(static_cast<int>(brightness * 0.6),
                     std::min(255, brightness), brightness));
    }

    for (const SkyMeteor& meteor : state.meteors) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(150, 240, 220));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, static_cast<int>(meteor.x), static_cast<int>(meteor.y),
                 nullptr);
        LineTo(dc, static_cast<int>(meteor.x - meteor.vx * 10.0f),
               static_cast<int>(meteor.y - meteor.vy * 10.0f));
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
}

void DrawAnimeSky(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(26, 36, 78), RGB(232, 150, 152));

    FillRadialGlow(dc, static_cast<int>(width * 0.68),
                   static_cast<int>(height * 0.60),
                   static_cast<int>(std::max(width, height) * 0.42),
                   RGB(255, 204, 176), 110);
    FillRadialGlow(dc, static_cast<int>(width * 0.30),
                   static_cast<int>(height * 0.34),
                   static_cast<int>(std::max(width, height) * 0.36),
                   RGB(150, 158, 224), 80);

    for (const SkyMeteor& meteor : state.meteors) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 186, 210));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, static_cast<int>(meteor.x), static_cast<int>(meteor.y),
                 nullptr);
        LineTo(dc, static_cast<int>(meteor.x - meteor.vx * 14.0f),
               static_cast<int>(meteor.y - meteor.vy * 14.0f));
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    for (const SkyPetal& cloud : state.petals) {
        if (cloud.shade >= 2) {
            continue;
        }
        const int cx = static_cast<int>(cloud.x);
        const int cy = static_cast<int>(cloud.y);
        const int rx = static_cast<int>(cloud.size);
        const int ry = std::max(3, static_cast<int>(cloud.size * 0.42f));
        const COLORREF top = cloud.shade == 1 ? RGB(255, 222, 232)
                                              : RGB(252, 246, 250);
        const COLORREF base = cloud.shade == 1 ? RGB(206, 156, 190)
                                               : RGB(188, 172, 216);
        FillSoftEllipse(dc, cx, cy, rx, ry, top, base);
        FillSoftEllipse(dc, cx - rx / 2, cy + ry / 3, rx / 2 + 1,
                        ry * 2 / 3 + 1, top, base);
        FillSoftEllipse(dc, cx + rx / 3, cy + ry / 4, rx * 2 / 5 + 1,
                        ry * 3 / 5 + 1, top, base);
    }

    const int horizon = height - std::max(44, height / 5);
    const int ridgeAmp = std::max(10, height / 18);

    std::vector<POINT> ridge;
    ridge.reserve(static_cast<size_t>(width / 16 + 4));
    for (int x = 0; x <= width + 16; x += 16) {
        const double wave =
            std::sin(x * 0.007 + 1.1) * 0.5 + std::sin(x * 0.017 + 0.4) * 0.5;
        const int y = horizon - ridgeAmp / 2 -
                      static_cast<int>(ridgeAmp * (0.55 + 0.45 * wave));
        ridge.push_back(POINT{x, y});
    }
    ridge.push_back(POINT{width, height});
    ridge.push_back(POINT{0, height});

    HBRUSH ridgeBrush = CreateSolidBrush(RGB(36, 42, 78));
    HGDIOBJ oldBrush = SelectObject(dc, ridgeBrush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, ridge.data(), static_cast<int>(ridge.size()));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(ridgeBrush);

    const int cityBase = horizon + std::max(2, height / 90);
    HBRUSH cityBrush = CreateSolidBrush(RGB(24, 28, 56));
    for (int x = 0; x < width; x += 42) {
        const int h = 12 + ((x * 37 + 23) % 48);
        const int right = std::min(width, x + 32);
        RECT building{x, cityBase - h, right, cityBase + 2};
        FillRect(dc, &building, cityBrush);
    }
    DeleteObject(cityBrush);

    const int towerX = static_cast<int>(width * 0.16);
    const int towerTop = horizon - std::max(50, height / 5);
    HPEN towerPen = CreatePen(PS_SOLID, 2, RGB(26, 30, 58));
    HGDIOBJ towerOld = SelectObject(dc, towerPen);
    MoveToEx(dc, towerX - 14, cityBase, nullptr);
    LineTo(dc, towerX, towerTop);
    LineTo(dc, towerX + 14, cityBase);
    for (int i = 1; i <= 3; ++i) {
        const int y = towerTop + (cityBase - towerTop) * i / 4;
        const int w = 14 * i / 4;
        MoveToEx(dc, towerX - w, y, nullptr);
        LineTo(dc, towerX + w, y);
    }
    SelectObject(dc, towerOld);
    DeleteObject(towerPen);
    StrokeCircle(dc, towerX, towerTop, 6, 3, RGB(26, 30, 58), 2);

    HPEN birdPen = CreatePen(PS_SOLID, 1, RGB(34, 36, 64));
    HGDIOBJ birdOld = SelectObject(dc, birdPen);
    for (const SkyPetal& bird : state.petals) {
        if (bird.shade != 2) {
            continue;
        }
        const int cx = static_cast<int>(bird.x);
        const int cy = static_cast<int>(bird.y);
        const int s = std::max(2, static_cast<int>(bird.size));
        POINT wing[3] = {POINT{cx - s * 2, cy + s / 2}, POINT{cx, cy - s / 2},
                         POINT{cx + s * 2, cy + s / 2}};
        Polyline(dc, wing, 3);
    }
    SelectObject(dc, birdOld);
    DeleteObject(birdPen);
}

void DrawStarTower(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(6, 10, 30), RGB(16, 28, 56));
    DrawStars(dc, state, 175);

    for (int i = 0; i < 340; ++i) {
        const double t = static_cast<double>(i) / 340.0;
        const int gx = static_cast<int>(width * 0.02 + t * width * 0.96);
        const int gy = static_cast<int>(height * 0.46 - t * height * 0.22 +
                                        std::sin(t * 8.0) * height * 0.02);
        const int shade = i % 7;
        const COLORREF color = shade == 0 ? RGB(232, 240, 255)
                              : (shade < 3 ? RGB(176, 190, 242)
                                           : RGB(112, 128, 192));
        SetPixel(dc, gx, gy, color);
        if (i % 3 == 0) {
            SetPixel(dc, gx, gy + 1, color);
        }
    }

    const int baseY = static_cast<int>(height * 0.86);
    const int topY = static_cast<int>(height * 0.30);
    const int towerX = static_cast<int>(width * 0.34);
    const int halfBase = std::max(16, width / 20);
    const int halfTop = std::max(3, halfBase / 6);

    auto legX = [&](int y, int side) {
        const double t = static_cast<double>(baseY - y) /
                         static_cast<double>(std::max(1, baseY - topY));
        return towerX +
               side * static_cast<int>(halfBase + (halfTop - halfBase) * t);
    };

    HPEN pen = CreatePen(PS_SOLID, 2, RGB(52, 66, 108));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, legX(baseY, -1), baseY, nullptr);
    LineTo(dc, legX(topY, -1), topY);
    MoveToEx(dc, legX(baseY, 1), baseY, nullptr);
    LineTo(dc, legX(topY, 1), topY);
    for (int k = 1; k <= 6; ++k) {
        const int y = baseY - (baseY - topY) * k / 7;
        const int y2 = baseY - (baseY - topY) * (k + 1) / 7;
        MoveToEx(dc, legX(y, -1), y, nullptr);
        LineTo(dc, legX(y, 1), y);
        MoveToEx(dc, legX(y, -1), y, nullptr);
        LineTo(dc, legX(y2, 1), y2);
        MoveToEx(dc, legX(y, 1), y, nullptr);
        LineTo(dc, legX(y2, -1), y2);
    }
    const int antennaTop = topY - std::max(18, height / 10);
    MoveToEx(dc, towerX, topY, nullptr);
    LineTo(dc, towerX, antennaTop);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    StrokeCircle(dc, towerX, topY + 8, std::max(8, halfTop * 3),
                 std::max(3, halfTop), RGB(96, 116, 174), 2);
    FillSoftCircle(dc, towerX, antennaTop, 3, RGB(236, 242, 255));

    HBRUSH treeBrush = CreateSolidBrush(RGB(5, 9, 18));
    HGDIOBJ oldTreeBrush = SelectObject(dc, treeBrush);
    HGDIOBJ oldTreePen = SelectObject(dc, GetStockObject(NULL_PEN));
    for (int x = -26; x < width + 26; x += 24) {
        const int h = 18 + ((x * 29 + 17) % 40);
        POINT tree[3] = {POINT{x, baseY - h}, POINT{x + 12, baseY - h - 22},
                         POINT{x + 24, baseY - h}};
        Polygon(dc, tree, 3);
    }
    SelectObject(dc, oldTreePen);
    SelectObject(dc, oldTreeBrush);
    DeleteObject(treeBrush);

    RECT ground{0, baseY, width, height};
    HBRUSH groundBrush = CreateSolidBrush(RGB(5, 9, 18));
    FillRect(dc, &ground, groundBrush);
    DeleteObject(groundBrush);
}

void DrawDuskBridge(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(36, 72, 146), RGB(250, 184, 186));
    FillRadialGlow(dc, static_cast<int>(width * 0.64),
                   static_cast<int>(height * 0.60),
                   static_cast<int>(std::max(width, height) * 0.46),
                   RGB(255, 208, 186), 100);

    for (const SkyPetal& cloud : state.petals) {
        const int cx = static_cast<int>(cloud.x);
        const int cy = static_cast<int>(cloud.y);
        const int rx = static_cast<int>(cloud.size);
        const int ry = std::max(3, static_cast<int>(cloud.size * 0.24f));
        FillSoftEllipse(dc, cx, cy, rx, ry, RGB(238, 224, 252),
                        RGB(178, 172, 228));
    }

    HPEN trail = CreatePen(PS_SOLID, 2, RGB(255, 198, 222));
    HGDIOBJ oldTrail = SelectObject(dc, trail);
    MoveToEx(dc, static_cast<int>(width * 0.28),
             static_cast<int>(height * 0.44), nullptr);
    LineTo(dc, static_cast<int>(width * 0.56),
           static_cast<int>(height * 0.19));
    MoveToEx(dc, static_cast<int>(width * 0.41),
             static_cast<int>(height * 0.50), nullptr);
    LineTo(dc, static_cast<int>(width * 0.70),
           static_cast<int>(height * 0.24));
    SelectObject(dc, oldTrail);
    DeleteObject(trail);

    const int groundY = static_cast<int>(height * 0.84);
    const int skyline = groundY - std::max(16, height / 22);

    HBRUSH building = CreateSolidBrush(RGB(58, 48, 88));
    for (int x = 0; x < width; x += 26) {
        const int h = 10 + ((x * 41 + 13) % std::max(12, height / 7));
        RECT rc{x, skyline - h, std::min(width, x + 22), groundY};
        FillRect(dc, &rc, building);
    }
    DeleteObject(building);

    const int towerX = static_cast<int>(width * 0.63);
    const int towerTop = skyline - std::max(46, height / 7);
    HPEN towerPen = CreatePen(PS_SOLID, 3, RGB(58, 48, 88));
    HGDIOBJ oldTower = SelectObject(dc, towerPen);
    MoveToEx(dc, towerX, skyline, nullptr);
    LineTo(dc, towerX, towerTop);
    MoveToEx(dc, towerX - 12, skyline, nullptr);
    LineTo(dc, towerX, towerTop + 20);
    MoveToEx(dc, towerX + 12, skyline, nullptr);
    LineTo(dc, towerX, towerTop + 20);
    SelectObject(dc, oldTower);
    DeleteObject(towerPen);
    FillSoftCircle(dc, towerX, skyline - 10, 9, RGB(58, 48, 88));
    FillSoftCircle(dc, towerX, towerTop + 12, 6, RGB(58, 48, 88));

    auto drawLamp = [&](int lx, int dir) {
        const int poleTop = groundY - static_cast<int>(height * 0.34);
        HPEN pole = CreatePen(PS_SOLID, 3, RGB(44, 36, 66));
        HGDIOBJ op = SelectObject(dc, pole);
        MoveToEx(dc, lx, groundY, nullptr);
        LineTo(dc, lx, poleTop);
        MoveToEx(dc, lx, poleTop, nullptr);
        LineTo(dc, lx + 22 * dir, poleTop);
        SelectObject(dc, op);
        DeleteObject(pole);
        const int lampX = lx + 22 * dir;
        const int lampY = poleTop + 4;
        FillRadialGlow(dc, lampX, lampY, 34, RGB(255, 224, 170), 140);
        FillSoftCircle(dc, lampX, lampY, 5, RGB(255, 240, 200));
    };
    drawLamp(static_cast<int>(width * 0.11), 1);
    drawLamp(static_cast<int>(width * 0.89), -1);

    const int bx = static_cast<int>(width * 0.14);
    const int by = groundY - 8;
    HPEN bike = CreatePen(PS_SOLID, 2, RGB(40, 32, 62));
    HGDIOBJ oldBike = SelectObject(dc, bike);
    MoveToEx(dc, bx, by, nullptr);
    LineTo(dc, bx + 13, by - 16);
    LineTo(dc, bx + 26, by);
    MoveToEx(dc, bx + 13, by - 16, nullptr);
    LineTo(dc, bx + 13, by - 34);
    SelectObject(dc, oldBike);
    DeleteObject(bike);
    StrokeCircle(dc, bx, by, 8, 8, RGB(40, 32, 62), 2);
    StrokeCircle(dc, bx + 26, by, 8, 8, RGB(40, 32, 62), 2);
    FillSoftCircle(dc, bx + 13, by - 38, 5, RGB(40, 32, 62));

    RECT ground{0, groundY, width, height};
    HBRUSH groundBrush = CreateSolidBrush(RGB(42, 32, 62));
    FillRect(dc, &ground, groundBrush);
    DeleteObject(groundBrush);
}

void DrawSummerRoad(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(24, 66, 128), RGB(148, 192, 212));
    FillRadialGlow(dc, static_cast<int>(width * 0.30),
                   static_cast<int>(height * 0.28),
                   static_cast<int>(std::max(width, height) * 0.36),
                   RGB(214, 234, 255), 70);

    for (const SkyPetal& cloud : state.petals) {
        const int cx = static_cast<int>(cloud.x);
        const int cy = static_cast<int>(cloud.y);
        const int rx = static_cast<int>(cloud.size);
        const int ry = std::max(6, static_cast<int>(cloud.size * 0.55f));
        FillSoftEllipse(dc, cx, cy, rx, ry, RGB(252, 252, 250),
                        RGB(186, 204, 226));
        FillSoftEllipse(dc, cx - rx / 2, cy + ry / 3, rx / 2 + 1,
                        ry * 2 / 3 + 1, RGB(250, 250, 248),
                        RGB(182, 200, 222));
        FillSoftEllipse(dc, cx + rx / 3, cy + ry / 4, rx * 2 / 5 + 1,
                        ry / 2 + 1, RGB(254, 254, 252), RGB(190, 208, 228));
    }

    const int horizon = static_cast<int>(height * 0.58);

    std::vector<POINT> hills;
    for (int x = 0; x <= width + 14; x += 14) {
        const int y = horizon - 10 -
                      static_cast<int>(20 * (0.5 +
                                             0.5 * std::sin(x * 0.010 + 0.8)));
        hills.push_back(POINT{x, y});
    }
    hills.push_back(POINT{width, horizon});
    hills.push_back(POINT{0, horizon});
    HBRUSH hillBrush = CreateSolidBrush(RGB(40, 76, 88));
    HGDIOBJ oldHills = SelectObject(dc, hillBrush);
    HGDIOBJ oldHillPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, hills.data(), static_cast<int>(hills.size()));
    SelectObject(dc, oldHillPen);
    SelectObject(dc, oldHills);
    DeleteObject(hillBrush);

    RECT grass{0, horizon, width, height};
    HBRUSH grassBrush = CreateSolidBrush(RGB(38, 78, 62));
    FillRect(dc, &grass, grassBrush);
    DeleteObject(grassBrush);

    POINT road[4] = {POINT{static_cast<int>(width * 0.455), horizon},
                     POINT{static_cast<int>(width * 0.545), horizon},
                     POINT{width, height}, POINT{0, height}};
    HBRUSH roadBrush = CreateSolidBrush(RGB(66, 70, 76));
    HGDIOBJ oldRoad = SelectObject(dc, roadBrush);
    HGDIOBJ oldRoadPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, road, 4);
    SelectObject(dc, oldRoadPen);
    SelectObject(dc, oldRoad);
    DeleteObject(roadBrush);

    HBRUSH dashBrush = CreateSolidBrush(RGB(232, 236, 238));
    for (int i = 1; i < 9; ++i) {
        const double t = static_cast<double>(i) / 9.0;
        const int y0 = horizon + static_cast<int>((height - horizon) * t);
        const int y1 = y0 + std::max(2, (height - horizon) / 22);
        POINT dash[4] = {POINT{width / 2 - 2, y0}, POINT{width / 2 + 2, y0},
                         POINT{width / 2 + 3, y1}, POINT{width / 2 - 3, y1}};
        HGDIOBJ ob = SelectObject(dc, dashBrush);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, dash, 4);
        SelectObject(dc, op);
        SelectObject(dc, ob);
    }
    DeleteObject(dashBrush);

    auto drawPylon = [&](int cx, int groundY, int topY, int halfW) {
        HPEN p = CreatePen(PS_SOLID, 2, RGB(48, 52, 58));
        HGDIOBJ op = SelectObject(dc, p);
        MoveToEx(dc, cx - halfW, groundY, nullptr);
        LineTo(dc, cx, topY);
        LineTo(dc, cx + halfW, groundY);
        for (int k = 1; k <= 4; ++k) {
            const int y = groundY - (groundY - topY) * k / 5;
            const int w = halfW - halfW * k / 5;
            MoveToEx(dc, cx - w, y, nullptr);
            LineTo(dc, cx + w, y);
        }
        const int arm = halfW + halfW / 2;
        for (int k = 0; k < 2; ++k) {
            const int y = topY + 14 + k * 14;
            MoveToEx(dc, cx - arm + k * 3, y, nullptr);
            LineTo(dc, cx + arm - k * 3, y);
        }
        SelectObject(dc, op);
        DeleteObject(p);
    };
    const int leftBase = horizon + static_cast<int>(height * 0.06);
    const int rightBase = horizon + static_cast<int>(height * 0.10);
    const int leftTop = horizon - static_cast<int>(height * 0.20);
    const int rightTop = horizon - static_cast<int>(height * 0.15);
    drawPylon(static_cast<int>(width * 0.30), leftBase, leftTop, 26);
    drawPylon(static_cast<int>(width * 0.72), rightBase, rightTop, 22);

    HPEN cable = CreatePen(PS_SOLID, 1, RGB(58, 62, 68));
    HGDIOBJ oldCable = SelectObject(dc, cable);
    for (int k = 0; k < 2; ++k) {
        const int x0 = static_cast<int>(width * 0.30) - 39 + k * 3;
        const int x1 = static_cast<int>(width * 0.72) - 33 + k * 3;
        const int y0 = leftTop + 14 + k * 14;
        const int y1 = rightTop + 14 + k * 14;
        bool first = true;
        for (int s = 0; s <= 24; ++s) {
            const double t = s / 24.0;
            const int px = x0 + static_cast<int>((x1 - x0) * t);
            const int py = y0 + static_cast<int>((y1 - y0) * t +
                                                 std::sin(t * kPi) * height * 0.045);
            if (first) {
                MoveToEx(dc, px, py, nullptr);
                first = false;
            } else {
                LineTo(dc, px, py);
            }
        }
    }
    SelectObject(dc, oldCable);
    DeleteObject(cable);

    HBRUSH gate = CreateSolidBrush(RGB(196, 200, 204));
    for (int k = 0; k < 6; ++k) {
        RECT bar{static_cast<int>(width * 0.44) + k * 6,
                 horizon + static_cast<int>(height * 0.02),
                 static_cast<int>(width * 0.44) + k * 6 + 3,
                 horizon + static_cast<int>(height * 0.06)};
        FillRect(dc, &bar, gate);
    }
    DeleteObject(gate);
}

void DrawNightBoat(HDC dc, const SkyState& state, int width, int height) {
    const int waterY = static_cast<int>(height * 0.64);
    VGradient(dc, width, waterY, RGB(8, 14, 34), RGB(18, 40, 66));
    RECT water{0, waterY, width, height};
    HBRUSH waterBrush = CreateSolidBrush(RGB(6, 22, 38));
    FillRect(dc, &water, waterBrush);
    DeleteObject(waterBrush);

    DrawStars(dc, state, 165);

    for (const SkyStar& star : state.stars) {
        const int rx = static_cast<int>(star.x);
        const int span = std::max(1, height - waterY - 10);
        const int ry = waterY + (static_cast<int>(star.y) * 37 % span) + 6;
        SetPixel(dc, rx, ry, RGB(58, 88, 128));
    }

    POINT leftMount[3] = {POINT{0, waterY},
                          POINT{static_cast<int>(width * 0.18),
                                waterY - static_cast<int>(height * 0.13)},
                          POINT{static_cast<int>(width * 0.36), waterY}};
    POINT rightMount[3] = {POINT{static_cast<int>(width * 0.62), waterY},
                           POINT{static_cast<int>(width * 0.82),
                                 waterY - static_cast<int>(height * 0.10)},
                           POINT{width, waterY}};
    HBRUSH mountBrush = CreateSolidBrush(RGB(6, 12, 22));
    HGDIOBJ oldMount = SelectObject(dc, mountBrush);
    HGDIOBJ oldMountPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, leftMount, 3);
    Polygon(dc, rightMount, 3);
    SelectObject(dc, oldMountPen);
    SelectObject(dc, oldMount);
    DeleteObject(mountBrush);

    for (int k = 0; k < 12; ++k) {
        const int y = waterY + 8 + k * std::max(4, (height - waterY) / 14);
        if (y >= height) {
            break;
        }
        const int w = 60 + k * 14;
        HPEN wave = CreatePen(PS_SOLID, 1, RGB(24, 52, 74));
        HGDIOBJ op = SelectObject(dc, wave);
        MoveToEx(dc, width / 2 - w, y, nullptr);
        LineTo(dc, width / 2 + w, y);
        SelectObject(dc, op);
        DeleteObject(wave);
    }

    const int bx = width / 2;
    const int by = waterY + (height - waterY) / 2 - 6;
    POINT hull[4] = {POINT{bx - 26, by}, POINT{bx + 26, by},
                     POINT{bx + 17, by + 9}, POINT{bx - 17, by + 9}};
    HBRUSH hullBrush = CreateSolidBrush(RGB(18, 24, 34));
    HGDIOBJ oldHull = SelectObject(dc, hullBrush);
    HGDIOBJ oldHullPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, hull, 4);
    SelectObject(dc, oldHullPen);
    SelectObject(dc, oldHull);
    DeleteObject(hullBrush);

    RECT mast{bx - 1, by - 30, bx + 1, by};
    HBRUSH mastBrush = CreateSolidBrush(RGB(24, 30, 42));
    FillRect(dc, &mast, mastBrush);
    DeleteObject(mastBrush);

    FillRadialGlow(dc, bx, by - 16, 44, RGB(255, 176, 96), 150);
    FillSoftCircle(dc, bx, by - 16, 5, RGB(255, 206, 140));

    for (int k = 0; k < 9; ++k) {
        const int w = 18 - k * 2;
        const int y = by + 14 + k * 3;
        HPEN glow = CreatePen(PS_SOLID, 1,
                              LerpColor(RGB(255, 168, 88), RGB(8, 26, 44),
                                        static_cast<double>(k) / 9.0));
        HGDIOBJ op = SelectObject(dc, glow);
        MoveToEx(dc, bx - w, y, nullptr);
        LineTo(dc, bx + w, y);
        SelectObject(dc, op);
        DeleteObject(glow);
    }
}


void DrawForestBridge(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(250, 194, 112), RGB(92, 54, 42));
    FillRadialGlow(dc, static_cast<int>(width * 0.52),
                   static_cast<int>(height * 0.40),
                   static_cast<int>(std::max(width, height) * 0.46),
                   RGB(255, 226, 150), 120);
    FillSoftCircle(dc, static_cast<int>(width * 0.52),
                   static_cast<int>(height * 0.40),
                   std::max(10, height / 30), RGB(255, 244, 206));

    const int waterY = static_cast<int>(height * 0.70);

    std::vector<POINT> ridge;
    for (int x = 0; x <= width + 16; x += 16) {
        const int y = waterY - 22 -
                      static_cast<int>(26 * (0.5 +
                                             0.5 * std::sin(x * 0.009 + 0.6)));
        ridge.push_back(POINT{x, y});
    }
    ridge.push_back(POINT{width, waterY});
    ridge.push_back(POINT{0, waterY});
    HBRUSH ridgeBrush = CreateSolidBrush(RGB(120, 76, 58));
    HGDIOBJ oldRidge = SelectObject(dc, ridgeBrush);
    HGDIOBJ oldRidgePen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, ridge.data(), static_cast<int>(ridge.size()));
    SelectObject(dc, oldRidgePen);
    SelectObject(dc, oldRidge);
    DeleteObject(ridgeBrush);

    RECT water{0, waterY, width, height};
    HBRUSH waterBrush = CreateSolidBrush(RGB(96, 58, 42));
    FillRect(dc, &water, waterBrush);
    DeleteObject(waterBrush);

    for (int k = 0; k < 10; ++k) {
        const int y = waterY + 10 + k * std::max(5, (height - waterY) / 11);
        if (y >= height) {
            break;
        }
        const int w = 50 + k * 18;
        HPEN glint = CreatePen(PS_SOLID, 1,
                               LerpColor(RGB(255, 214, 150), RGB(120, 70, 50),
                                         static_cast<double>(k) / 10.0));
        HGDIOBJ op = SelectObject(dc, glint);
        MoveToEx(dc, width / 2 - w, y, nullptr);
        LineTo(dc, width / 2 + w, y);
        SelectObject(dc, op);
        DeleteObject(glint);
    }

    const int bridgeTop = static_cast<int>(height * 0.56);
    const int bridgeBottom = static_cast<int>(height * 0.66);
    RECT body{static_cast<int>(width * 0.14), bridgeTop,
              static_cast<int>(width * 0.86), bridgeBottom};
    HBRUSH bodyBrush = CreateSolidBrush(RGB(70, 42, 32));
    FillRect(dc, &body, bodyBrush);
    DeleteObject(bodyBrush);

    HBRUSH holeBrush = CreateSolidBrush(RGB(38, 24, 20));
    for (int i = 0; i < 3; ++i) {
        const int cx = static_cast<int>(width * (0.30 + 0.20 * i));
        const int rx = static_cast<int>(width * 0.055);
        const int ry = bridgeBottom - bridgeTop - 2;
        POINT arch[13];
        for (int s = 0; s < 13; ++s) {
            const double a = kPi * s / 12.0;
            arch[s] = POINT{cx - static_cast<int>(std::cos(a) * rx),
                            bridgeBottom - static_cast<int>(std::sin(a) * ry)};
        }
        HGDIOBJ ob = SelectObject(dc, holeBrush);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, arch, 13);
        SelectObject(dc, op);
        SelectObject(dc, ob);
    }
    DeleteObject(holeBrush);

    HPEN rail = CreatePen(PS_SOLID, 2, RGB(54, 32, 24));
    HGDIOBJ oldRail = SelectObject(dc, rail);
    MoveToEx(dc, static_cast<int>(width * 0.14), bridgeTop - 10, nullptr);
    LineTo(dc, static_cast<int>(width * 0.86), bridgeTop - 10);
    for (int i = 0; i <= 12; ++i) {
        const int x = static_cast<int>(width * (0.14 + 0.06 * i));
        MoveToEx(dc, x, bridgeTop - 10, nullptr);
        LineTo(dc, x, bridgeTop + 2);
    }
    SelectObject(dc, oldRail);
    DeleteObject(rail);

    auto pine = [&](int cx, int baseY, int h, int halfW) {
        HBRUSH b = CreateSolidBrush(RGB(46, 28, 22));
        HGDIOBJ ob = SelectObject(dc, b);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        for (int layer = 0; layer < 3; ++layer) {
            const int top = baseY - h + layer * h / 5;
            const int bottom = top + h * 2 / 5;
            const int w = halfW * (2 + layer) / 4;
            POINT tri[3] = {POINT{cx, top}, POINT{cx - w, bottom},
                            POINT{cx + w, bottom}};
            Polygon(dc, tri, 3);
        }
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(b);
    };
    pine(static_cast<int>(width * 0.06), height, static_cast<int>(height * 0.62),
         static_cast<int>(width * 0.10));
    pine(static_cast<int>(width * 0.16), height, static_cast<int>(height * 0.48),
         static_cast<int>(width * 0.08));
    pine(static_cast<int>(width * 0.95), height, static_cast<int>(height * 0.58),
         static_cast<int>(width * 0.09));
}

void DrawDesertNight(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(20, 14, 50), RGB(232, 112, 116));
    DrawStars(dc, state, 180);

    FillRadialGlow(dc, static_cast<int>(width * 0.30),
                   static_cast<int>(height * 0.58),
                   static_cast<int>(std::max(width, height) * 0.18),
                   RGB(255, 200, 180), 140);
    FillSoftCircle(dc, static_cast<int>(width * 0.30),
                   static_cast<int>(height * 0.58),
                   std::max(8, height / 45), RGB(255, 236, 210));

    for (int i = 0; i < 4; ++i) {
        const double t = i / 3.0;
        FillSoftEllipse(dc, static_cast<int>(width * (0.18 + 0.22 * i)),
                        static_cast<int>(height * (0.50 + 0.04 * i)),
                        static_cast<int>(width * 0.16),
                        std::max(3, static_cast<int>(height * 0.02)),
                        RGB(255, 176, 164), RGB(186, 86, 118));
    }

    const int baseY = static_cast<int>(height * 0.82);
    auto monolith = [&](int cx, int base, int w, int h) {
        RECT body{cx - w / 2, base - h, cx + w / 2, base};
        HBRUSH b = CreateSolidBrush(RGB(36, 20, 50));
        FillRect(dc, &body, b);
        DeleteObject(b);
        FillSoftEllipse(dc, cx, base - h, w / 2,
                        std::max(4, w / 8), RGB(44, 26, 60),
                        RGB(26, 14, 38));
    };
    monolith(static_cast<int>(width * 0.18), baseY,
             static_cast<int>(width * 0.10), static_cast<int>(height * 0.24));
    monolith(static_cast<int>(width * 0.32), baseY,
             static_cast<int>(width * 0.07), static_cast<int>(height * 0.16));
    monolith(static_cast<int>(width * 0.78), baseY,
             static_cast<int>(width * 0.12), static_cast<int>(height * 0.20));

    RECT river{0, baseY, width, height};
    HBRUSH riverBrush = CreateSolidBrush(RGB(18, 10, 30));
    FillRect(dc, &river, riverBrush);
    DeleteObject(riverBrush);

    for (int k = 0; k < 8; ++k) {
        const int y = baseY + 8 + k * std::max(4, (height - baseY) / 9);
        if (y >= height) {
            break;
        }
        HPEN glint = CreatePen(PS_SOLID, 1, RGB(196, 106, 130));
        HGDIOBJ op = SelectObject(dc, glint);
        MoveToEx(dc, width / 2 - 80 - k * 10, y, nullptr);
        LineTo(dc, width / 2 + 80 + k * 10, y);
        SelectObject(dc, op);
        DeleteObject(glint);
    }
}

void DrawNeonCity(HDC dc, const SkyState& state, int width, int height) {
    const int horizon = static_cast<int>(height * 0.76);
    VGradient(dc, width, height, RGB(18, 8, 36), RGB(196, 74, 76));

    FillRadialGlow(dc, width / 2, horizon,
                   static_cast<int>(std::max(width, height) * 0.50),
                   RGB(255, 150, 92), 120);

    std::vector<POINT> hills;
    hills.push_back(POINT{0, horizon});
    for (int x = 0; x <= width + 20; x += 20) {
        const int y = horizon - 24 -
                      static_cast<int>(28 * (0.5 +
                                             0.5 * std::sin(x * 0.008 + 0.5)));
        hills.push_back(POINT{x, y});
    }
    hills.push_back(POINT{width, horizon});
    HBRUSH hillBrush = CreateSolidBrush(RGB(60, 26, 54));
    HGDIOBJ oldHill = SelectObject(dc, hillBrush);
    HGDIOBJ oldHillPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, hills.data(), static_cast<int>(hills.size()));
    SelectObject(dc, oldHillPen);
    SelectObject(dc, oldHill);
    DeleteObject(hillBrush);

    HBRUSH building = CreateSolidBrush(RGB(30, 14, 46));
    for (int x = 0; x < width; x += 28) {
        int h = 14 + ((x * 53 + 7) % std::max(18, height / 6));
        if (x > static_cast<int>(width * 0.38) &&
            x < static_cast<int>(width * 0.62)) {
            h += height / 10;
        }
        RECT rc{x, horizon - h, std::min(width, x + 24), horizon};
        FillRect(dc, &rc, building);
    }
    DeleteObject(building);

    const int towerX = width / 2;
    const int towerH = static_cast<int>(height * 0.34);
    RECT tower{towerX - static_cast<int>(width * 0.030), horizon - towerH,
               towerX + static_cast<int>(width * 0.030), horizon};
    HBRUSH towerBrush = CreateSolidBrush(RGB(38, 18, 52));
    FillRect(dc, &tower, towerBrush);
    DeleteObject(towerBrush);

    POINT crown[3] = {POINT{towerX, horizon - towerH -
                                          static_cast<int>(height * 0.06)},
                      POINT{towerX - static_cast<int>(width * 0.030),
                            horizon - towerH},
                      POINT{towerX + static_cast<int>(width * 0.030),
                            horizon - towerH}};
    HBRUSH crownBrush = CreateSolidBrush(RGB(46, 22, 62));
    HGDIOBJ oldCrown = SelectObject(dc, crownBrush);
    HGDIOBJ oldCrownPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, crown, 3);
    SelectObject(dc, oldCrownPen);
    SelectObject(dc, oldCrown);
    DeleteObject(crownBrush);

    HPEN towerEdge = CreatePen(PS_SOLID, 2, RGB(255, 140, 84));
    HGDIOBJ oldTowerEdge = SelectObject(dc, towerEdge);
    MoveToEx(dc, tower.left, horizon, nullptr);
    LineTo(dc, tower.left, horizon - towerH);
    MoveToEx(dc, tower.right - 1, horizon, nullptr);
    LineTo(dc, tower.right - 1, horizon - towerH);
    MoveToEx(dc, towerX, horizon - towerH, nullptr);
    LineTo(dc, towerX, horizon - towerH - static_cast<int>(height * 0.06));
    SelectObject(dc, oldTowerEdge);
    DeleteObject(towerEdge);

    FillSoftCircle(dc, towerX,
                   horizon - towerH - static_cast<int>(height * 0.06), 4,
                   RGB(255, 214, 150));

    RECT ground{0, horizon, width, height};
    HBRUSH groundBrush = CreateSolidBrush(RGB(12, 5, 20));
    FillRect(dc, &ground, groundBrush);
    DeleteObject(groundBrush);

    FillRadialGlow(dc, width / 2, horizon + 6,
                   static_cast<int>(std::max(width, height) * 0.38),
                   RGB(255, 120, 78), 70);

    const double pulse = 0.5 + 0.5 * std::sin(state.time * 1.4);
    const COLORREF roadColor = LerpColor(RGB(196, 76, 44), RGB(255, 194, 96),
                                         pulse);
    HPEN road = CreatePen(PS_SOLID, 2, roadColor);
    HGDIOBJ oldRoad = SelectObject(dc, road);
    for (int i = -7; i <= 7; ++i) {
        const int x = width / 2 + i * width / 10;
        MoveToEx(dc, width / 2, horizon, nullptr);
        LineTo(dc, x, height);
    }
    for (int k = 1; k <= 5; ++k) {
        const double t = static_cast<double>(k) / 5.0;
        const int y = horizon + static_cast<int>((height - horizon) * t * t);
        MoveToEx(dc, 0, y, nullptr);
        LineTo(dc, width, y);
    }
    SelectObject(dc, oldRoad);
    DeleteObject(road);

    HBRUSH window = CreateSolidBrush(RGB(255, 176, 96));
    for (int x = 10; x < width; x += 28) {
        int h = 14 + ((x * 53 + 7) % std::max(18, height / 6));
        if (x > static_cast<int>(width * 0.38) &&
            x < static_cast<int>(width * 0.62)) {
            h += height / 10;
        }
        RECT lit{x + 4, horizon - h + 8, x + 10, horizon - h + 13};
        FillRect(dc, &lit, window);
        RECT lit2{x + 14, horizon - h + 20, x + 20, horizon - h + 25};
        FillRect(dc, &lit2, window);
    }
    DeleteObject(window);

    HBRUSH towerWindow = CreateSolidBrush(RGB(255, 204, 130));
    for (int k = 0; k < 6; ++k) {
        const int y = horizon - towerH + 18 + k * (towerH / 7);
        RECT lit{towerX - 8, y, towerX + 8, y + 4};
        FillRect(dc, &lit, towerWindow);
    }
    DeleteObject(towerWindow);
}

void DrawWindTurbine(HDC dc, const SkyState& state, int width, int height) {
    VGradient(dc, width, height, RGB(20, 60, 92), RGB(126, 192, 192));

    const int shipX = static_cast<int>(
        std::fmod(state.time * 1.6 + 60.0, static_cast<double>(width) + 300.0)) -
        150;
    const int shipY = static_cast<int>(height * 0.17);
    FillSoftEllipse(dc, shipX, shipY, 64, 12, RGB(210, 238, 244),
                    RGB(126, 172, 186));
    POINT fin[3] = {POINT{shipX - 40, shipY}, POINT{shipX - 22, shipY - 16},
                    POINT{shipX - 4, shipY}};
    HBRUSH finBrush = CreateSolidBrush(RGB(160, 200, 210));
    HGDIOBJ oldFin = SelectObject(dc, finBrush);
    HGDIOBJ oldFinPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, fin, 3);
    SelectObject(dc, oldFinPen);
    SelectObject(dc, oldFin);
    DeleteObject(finBrush);

    const int groundY = static_cast<int>(height * 0.78);
    FillSoftCircle(dc, static_cast<int>(width * 0.16), groundY,
                   std::max(18, width / 16), RGB(206, 222, 220));
    RECT tower{static_cast<int>(width * 0.80), groundY - static_cast<int>(height * 0.32),
               static_cast<int>(width * 0.87), groundY};
    HBRUSH towerBrush = CreateSolidBrush(RGB(196, 216, 216));
    HGDIOBJ oldTower = SelectObject(dc, towerBrush);
    HGDIOBJ oldTowerPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, tower.left, tower.top, tower.right, tower.bottom, 26, 26);
    SelectObject(dc, oldTowerPen);
    SelectObject(dc, oldTower);
    DeleteObject(towerBrush);

    RECT ground{0, groundY, width, height};
    HBRUSH groundBrush = CreateSolidBrush(RGB(150, 158, 74));
    FillRect(dc, &ground, groundBrush);
    DeleteObject(groundBrush);

    auto turbine = [&](int cx, int hubY, int len, double phase) {
        HPEN pole = CreatePen(PS_SOLID, 7, RGB(220, 238, 238));
        HGDIOBJ op = SelectObject(dc, pole);
        MoveToEx(dc, cx, hubY, nullptr);
        LineTo(dc, cx, groundY);
        SelectObject(dc, op);
        DeleteObject(pole);
        for (int b = 0; b < 3; ++b) {
            const double ang = phase + b * 2.0 * kPi / 3.0;
            HPEN blade = CreatePen(PS_SOLID, 5, RGB(232, 246, 244));
            HGDIOBJ ob = SelectObject(dc, blade);
            MoveToEx(dc, cx, hubY, nullptr);
            LineTo(dc, cx + static_cast<int>(std::cos(ang) * len),
                   hubY + static_cast<int>(std::sin(ang) * len));
            SelectObject(dc, ob);
            DeleteObject(blade);
        }
        FillSoftCircle(dc, cx, hubY, 7, RGB(242, 250, 250));
    };
    turbine(static_cast<int>(width * 0.32),
            static_cast<int>(height * 0.36), static_cast<int>(height * 0.22),
            state.time * 0.05);
    turbine(static_cast<int>(width * 0.66),
            static_cast<int>(height * 0.44), static_cast<int>(height * 0.17),
            state.time * 0.05 + 1.9);
}

struct ChampionHero {
    int cx;
    int footY;
    int h;
    int lean;
    int pose;
    bool crown;
    bool longHair;
    int weapon;
    int wing;
    bool spike;
};

extern "C" {
extern const unsigned char _binary_extreme_jpg_start[];
extern const unsigned char _binary_extreme_jpg_end[];
}

HBITMAP LoadExtremeBitmap() {
    static HBITMAP bitmap = nullptr;
    static bool tried = false;
    if (tried) {
        return bitmap;
    }
    tried = true;

    const void* data = _binary_extreme_jpg_start;
    const DWORD size = static_cast<DWORD>(_binary_extreme_jpg_end -
                                          _binary_extreme_jpg_start);
    if (size == 0) {
        return nullptr;
    }

    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_IWICImagingFactory,
                                   reinterpret_cast<void**>(&factory)))) {
        if (SUCCEEDED(factory->CreateStream(&stream))) {
            stream->InitializeFromMemory(
                reinterpret_cast<BYTE*>(const_cast<void*>(data)), size);
        }
        if (stream != nullptr &&
            SUCCEEDED(factory->CreateDecoderFromStream(
                stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) {
            decoder->GetFrame(0, &frame);
        }
        if (frame != nullptr &&
            SUCCEEDED(factory->CreateFormatConverter(&converter))) {
            converter->Initialize(frame, GUID_WICPixelFormat32bppBGR,
                                  WICBitmapDitherTypeNone, nullptr, 0.0,
                                  WICBitmapPaletteTypeCustom);
        }
        UINT imageWidth = 0;
        UINT imageHeight = 0;
        if (converter != nullptr &&
            SUCCEEDED(converter->GetSize(&imageWidth, &imageHeight)) &&
            imageWidth > 0 && imageHeight > 0) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(imageWidth);
            info.bmiHeader.biHeight = -static_cast<LONG>(imageHeight);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HDC screen = GetDC(nullptr);
            HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                           &bits, nullptr, 0);
            ReleaseDC(nullptr, screen);
            if (dib != nullptr && bits != nullptr) {
                const UINT stride = imageWidth * 4;
                if (SUCCEEDED(converter->CopyPixels(
                        nullptr, stride, stride * imageHeight,
                        static_cast<BYTE*>(bits)))) {
                    bitmap = dib;
                } else {
                    DeleteObject(dib);
                }
            }
        }
    }

    if (converter != nullptr) {
        converter->Release();
    }
    if (frame != nullptr) {
        frame->Release();
    }
    if (decoder != nullptr) {
        decoder->Release();
    }
    if (stream != nullptr) {
        stream->Release();
    }
    if (factory != nullptr) {
        factory->Release();
    }
    if (init == S_OK) {
        CoUninitialize();
    }
    return bitmap;
}

void DrawChampion(HDC dc, const SkyState& state, int width, int height) {
    static HBITMAP cache = nullptr;
    static int cacheWidth = 0;
    static int cacheHeight = 0;

    if (cache == nullptr || cacheWidth != width || cacheHeight != height) {
        if (cache != nullptr) {
            DeleteObject(cache);
            cache = nullptr;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        cache = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr,
                                 0);
        ReleaseDC(nullptr, screen);

        HDC cacheDc = CreateCompatibleDC(nullptr);
        HGDIOBJ oldCache = SelectObject(cacheDc, cache);
        HBITMAP image = LoadExtremeBitmap();
        if (image != nullptr) {
            BITMAP bitmapInfo{};
            GetObject(image, sizeof(bitmapInfo), &bitmapInfo);
            const double scale = std::max(
                static_cast<double>(width) / bitmapInfo.bmWidth,
                static_cast<double>(height) / bitmapInfo.bmHeight);
            const int drawWidth =
                static_cast<int>(bitmapInfo.bmWidth * scale);
            const int drawHeight =
                static_cast<int>(bitmapInfo.bmHeight * scale);
            const int drawX = (width - drawWidth) / 2;
            const int drawY = (height - drawHeight) / 2;
            HDC imageDc = CreateCompatibleDC(cacheDc);
            HGDIOBJ oldImage = SelectObject(imageDc, image);
            SetStretchBltMode(cacheDc, HALFTONE);
            SetBrushOrgEx(cacheDc, 0, 0, nullptr);
            StretchBlt(cacheDc, drawX, drawY, drawWidth, drawHeight, imageDc,
                       0, 0, bitmapInfo.bmWidth, bitmapInfo.bmHeight, SRCCOPY);
            SelectObject(imageDc, oldImage);
            DeleteDC(imageDc);
        } else {
            HBRUSH background = CreateSolidBrush(RGB(24, 26, 34));
            RECT full{0, 0, width, height};
            FillRect(cacheDc, &full, background);
            DeleteObject(background);
        }
        SelectObject(cacheDc, oldCache);
        DeleteDC(cacheDc);
        cacheWidth = width;
        cacheHeight = height;
    }

    if (cache == nullptr) {
        return;
    }

    HDC cacheDc = CreateCompatibleDC(dc);
    HGDIOBJ oldCache = SelectObject(cacheDc, cache);
    BitBlt(dc, 0, 0, width, height, cacheDc, 0, 0, SRCCOPY);
    SelectObject(cacheDc, oldCache);
    DeleteDC(cacheDc);

    for (int i = 0; i < 26; ++i) {
        const double seed = i * 0.6180339887;
        const double local = seed * 10.0 - std::floor(seed * 10.0);
        const int x = static_cast<int>(width * local);
        const double base = seed * 7.0 - std::floor(seed * 7.0);
        const int y = static_cast<int>(height * (0.18 + 0.62 * base)) +
                      static_cast<int>(std::sin(state.time * (0.3 + i % 5 * 0.05) +
                                                seed * 6.28) *
                                       12.0);
        const int radius = 2 + i % 4;
        FillRadialGlow(dc, x, y, radius * 4, RGB(150, 210, 255), 60);
        FillSoftCircle(dc, x, y, radius, RGB(226, 242, 255));
    }
}

}  // namespace

void ResetSky(SkyState& state) {
    state.stars.clear();
    state.meteors.clear();
    state.petals.clear();
    state.diskAngle = 0.0f;
    state.time = 0.0f;
    state.kind = -1;
    state.initialized = false;
}

void EnsureSky(SkyState& state, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    if (state.initialized && state.kind == g_theme) {
        return;
    }
    state.kind = g_theme;
    state.initialized = true;
    state.stars.clear();
    state.meteors.clear();
    state.petals.clear();
    state.diskAngle = 0.0f;
    state.time = 0.0f;
    SeedSky();

    auto makeStars = [&](int count) {
        for (int i = 0; i < count; ++i) {
            SkyStar star;
            star.x = static_cast<float>(rand() % std::max(1, width));
            star.y = static_cast<float>(rand() % std::max(1, height));
            star.vx = (static_cast<float>(rand() % 100) - 50.0f) / 260.0f;
            star.vy = (static_cast<float>(rand() % 60) - 20.0f) / 260.0f;
            star.size = 1 + rand() % 2;
            star.alpha = static_cast<BYTE>(120 + rand() % 136);
            state.stars.push_back(star);
        }
    };
    auto makePetals = [&](int count, float minSize, float maxSize,
                          float minVy, float maxVy, bool spreadAll) {
        for (int i = 0; i < count; ++i) {
            SkyPetal petal;
            petal.x = static_cast<float>(rand() % std::max(1, width));
            petal.y = spreadAll
                ? static_cast<float>(rand() % std::max(1, height + 60)) - 30.0f
                : static_cast<float>(rand() % std::max(1, height)) -
                      static_cast<float>(height);
            petal.vx = (static_cast<float>(rand() % 100) - 50.0f) / 150.0f;
            petal.vy = minVy + (maxVy - minVy) * (rand() % 100) / 100.0f;
            petal.angle = static_cast<float>(rand() % 628) / 100.0f;
            petal.spin = (static_cast<float>(rand() % 100) - 50.0f) / 1500.0f;
            petal.size = minSize +
                (maxSize - minSize) * (rand() % 100) / 100.0f;
            petal.phase = static_cast<float>(rand() % 600);
            petal.shade = rand() % 3;
            state.petals.push_back(petal);
        }
    };

    switch (g_theme) {
    case 2: // 星河璀璨
        makeStars(90);
        break;
    case 3: // 深渊黑洞
        makeStars(110);
        break;
    case 4: // 樱吹花落
        makePetals(92, 3.0f, 7.5f, 0.7f, 2.2f, true);
        break;
    case 5: { // 蔚蓝地球：星空 + 云/陆斑
        makeStars(130);
        const int rr = static_cast<int>(std::max(width, height) * 0.95);
        const int cy = height + static_cast<int>(rr * 0.66);
        const int top = cy - rr;
        for (int i = 0; i < 90; ++i) {
            SkyPetal cloud;
            cloud.x = static_cast<float>(rand() % std::max(1, width));
            const int span = std::max(1, height - top);
            cloud.y = static_cast<float>(top + rand() % span);
            const bool land = (rand() % 5 == 0);
            cloud.size = land ? static_cast<float>(22 + rand() % 26)
                              : static_cast<float>(4 + rand() % 10);
            cloud.shade = land ? 0 : 1;
            cloud.phase = static_cast<float>(rand() % 600);
            state.petals.push_back(cloud);
        }
        break;
    }
    case 6: // 极光雪原
        makeStars(70);
        makePetals(90, 1.0f, 2.6f, 0.4f, 1.3f, true);
        break;
    case 7: // 鎏金雨夜
        makePetals(150, 8.0f, 20.0f, 7.0f, 13.0f, true);
        break;
    case 8: // 流萤夏夜
        makePetals(46, 1.5f, 3.0f, 0.0f, 0.0f, true);
        break;
    case 9: // 落叶秋径
        makePetals(64, 5.0f, 11.0f, 0.9f, 2.4f, true);
        break;
    case 10: // 深海气泡
        makePetals(54, 3.0f, 12.0f, 0.4f, 1.6f, true);
        break;
    case 11: // 赛博霓虹
        makePetals(44, 3.0f, 8.0f, 0.0f, 0.0f, true);
        break;
    case 12: { // 矩阵代码雨：列
        const int spacing = 16;
        const int columns = std::max(1, width / spacing);
        for (int i = 0; i < columns; ++i) {
            SkyPetal col;
            col.x = static_cast<float>(i * spacing + 6);
            col.y = static_cast<float>(rand() % std::max(1, height));
            col.vy = 10.0f + static_cast<float>(rand() % 40) / 2.0f;
            col.size = static_cast<float>(8 + rand() % 12);
            col.phase = static_cast<float>(rand() % 1000);
            state.petals.push_back(col);
        }
        break;
    }
    case 13: // 星云旋涡
        makeStars(170);
        break;
    case 14: // 奶油午后
        makePetals(14, 40.0f, 90.0f, 0.0f, 0.0f, true);
        break;
    case 15: // 月光白橘
        makePetals(8, 70.0f, 150.0f, 0.0f, 0.0f, true);
        break;
    case 16: // 星空薄荷
        makeStars(110);
        break;
    case 17: { // 月球背面：圆内环形山
        makeStars(90);
        const int R = static_cast<int>(std::min(width, height) * 0.40);
        const int cx = width / 2;
        const int cy = height / 2;
        for (int i = 0; i < 52; ++i) {
            SkyPetal crater;
            const double ang = (rand() % 628) / 100.0;
            const double rad =
                std::sqrt((rand() % 1000) / 1000.0) * R * 0.90;
            crater.x = static_cast<float>(cx + std::cos(ang) * rad);
            crater.y = static_cast<float>(cy + std::sin(ang) * rad);
            crater.size = static_cast<float>(3 + rand() % 16);
            crater.shade = (rand() % 4 == 0) ? 0 : 1;
            crater.phase = static_cast<float>(rand() % 600);
            state.petals.push_back(crater);
        }
        break;
    }
    case 18: // 电路板纹
        break;
    case 19: // 深海鲸落
        makePetals(70, 1.0f, 2.2f, 0.3f, 1.0f, true);
        break;
    case 21: // 星塔夜航
        makeStars(130);
        break;
    case 22: // 暮色天桥
        makePetals(7, 40.0f, 92.0f, 0.0f, 0.0f, true);
        break;
    case 23: // 夏日云路
        makePetals(8, 52.0f, 110.0f, 0.0f, 0.0f, true);
        break;
    case 24: // 夜海孤舟
        makeStars(150);
        break;
    case 26: // 星漠夜穹
        makeStars(180);
        break;
    case 29: // 極：云雾
        makePetals(10, 50.0f, 130.0f, 0.0f, 0.0f, true);
        break;
    case 20: { // 青空物语：积雨云 + 飞鸟
        makePetals(20, 26.0f, 76.0f, 0.0f, 0.0f, true);
        for (SkyPetal& cloud : state.petals) {
            cloud.shade = (rand() % 3 == 0) ? 1 : 0;
            cloud.vx = (static_cast<float>(rand() % 100) - 20.0f) / 900.0f;
        }
        for (int i = 0; i < 9; ++i) {
            SkyPetal bird;
            bird.x = static_cast<float>(rand() % std::max(1, width));
            bird.y = static_cast<float>(height) * 0.18f +
                     static_cast<float>(rand() % std::max(1, height / 2));
            bird.vx = 0.6f + static_cast<float>(rand() % 40) / 100.0f;
            bird.vy = (static_cast<float>(rand() % 40) - 20.0f) / 200.0f;
            bird.size = 2.0f + static_cast<float>(rand() % 3);
            bird.phase = static_cast<float>(rand() % 600);
            bird.shade = 2;
            state.petals.push_back(bird);
        }
        break;
    }
    default:
        makeStars(90);
        break;
    }
}

void UpdateSky(SkyState& state, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    EnsureSky(state, width, height);
    state.time += 0.16f;

    auto updateStars = [&]() {
        for (SkyStar& star : state.stars) {
            star.x += star.vx;
            star.y += star.vy;
            if (star.x < 0.0f) star.x += static_cast<float>(width);
            if (star.x >= static_cast<float>(width)) {
                star.x -= static_cast<float>(width);
            }
            if (star.y < 0.0f) star.y += static_cast<float>(height);
            if (star.y >= static_cast<float>(height)) {
                star.y -= static_cast<float>(height);
            }
        }
    };

    switch (g_theme) {
    case 2: {
        updateStars();
        if (rand() % 100 < 5) {
            const int fw = std::max(1, width);
            const int fh = std::max(1, height);
            SkyMeteor meteor;
            meteor.x = -static_cast<float>(rand() % (fw / 2 + 1));
            meteor.y = static_cast<float>(rand() % (fh + fh / 3 + 1)) -
                       static_cast<float>(fh / 3);
            meteor.vx = 4.0f + static_cast<float>(rand() % 40) / 10.0f;
            meteor.vy = 2.0f + static_cast<float>(rand() % 20) / 10.0f;
            state.meteors.push_back(meteor);
        }
        for (SkyMeteor& meteor : state.meteors) {
            meteor.x += meteor.vx;
            meteor.y += meteor.vy;
        }
        const float limitX = static_cast<float>(width) + 320.0f;
        const float limitY = static_cast<float>(height) + 320.0f;
        state.meteors.erase(
            std::remove_if(state.meteors.begin(), state.meteors.end(),
                           [limitX, limitY](const SkyMeteor& meteor) {
                               return meteor.x > limitX || meteor.y > limitY;
                           }),
            state.meteors.end());
        break;
    }
    case 3: {
        updateStars();
        state.diskAngle += 0.045f;
        if (state.diskAngle > 2.0f * static_cast<float>(kPi)) {
            state.diskAngle -= 2.0f * static_cast<float>(kPi);
        }
        break;
    }
    case 4: { // 樱花
        for (SkyPetal& petal : state.petals) {
            petal.x += petal.vx +
                       std::sin((petal.y + petal.phase) * 0.02f) * 0.9f;
            petal.y += petal.vy;
            petal.angle += petal.spin;
            if (petal.y > height + 26.0f) {
                petal.y = -26.0f;
                petal.x = static_cast<float>(rand() % std::max(1, width));
            }
            if (petal.x < -26.0f) petal.x = static_cast<float>(width) + 20.0f;
            if (petal.x > width + 26.0f) petal.x = -20.0f;
        }
        break;
    }
    case 5: // 地球：星星漂移（云不动）
        updateStars();
        break;
    case 6: { // 极光 + 细雪
        updateStars();
        for (SkyPetal& snow : state.petals) {
            snow.x += std::sin((snow.y + snow.phase) * 0.03f) * 0.4f;
            snow.y += snow.vy;
            if (snow.y > height + 10.0f) {
                snow.y = -10.0f;
                snow.x = static_cast<float>(rand() % std::max(1, width));
            }
        }
        break;
    }
    case 7: { // 雨丝
        for (SkyPetal& rain : state.petals) {
            rain.x += 2.2f;
            rain.y += rain.vy;
            if (rain.y > height + 20.0f) {
                rain.y = -20.0f;
                rain.x = static_cast<float>(rand() % std::max(1, width + 200)) -
                         200.0f;
            }
            if (rain.x > width + 30.0f) {
                rain.x = -30.0f;
            }
        }
        break;
    }
    case 8: // 萤火虫：位置由绘制时的 sin 决定
        break;
    case 9: { // 枫叶
        for (SkyPetal& leaf : state.petals) {
            leaf.x += leaf.vx +
                      std::sin((leaf.y + leaf.phase) * 0.02f) * 1.2f;
            leaf.y += leaf.vy;
            leaf.angle += leaf.spin;
            if (leaf.y > height + 30.0f) {
                leaf.y = -30.0f;
                leaf.x = static_cast<float>(rand() % std::max(1, width));
            }
            if (leaf.x < -30.0f) leaf.x = static_cast<float>(width) + 24.0f;
            if (leaf.x > width + 30.0f) leaf.x = -24.0f;
        }
        break;
    }
    case 10: { // 气泡上浮
        for (SkyPetal& bubble : state.petals) {
            bubble.y -= bubble.vy;
            bubble.x += std::sin((bubble.y + bubble.phase) * 0.03f) * 0.5f;
            if (bubble.y < -20.0f) {
                bubble.y = static_cast<float>(height) + 20.0f;
                bubble.x = static_cast<float>(rand() % std::max(1, width));
            }
        }
        break;
    }
    case 11: { // 霓虹粒子横向扫过
        for (SkyPetal& particle : state.petals) {
            particle.x += 5.0f + particle.size * 0.6f;
            if (particle.x > width + 40.0f) {
                particle.x = -40.0f;
                particle.y = static_cast<float>(rand() % std::max(1, height));
            }
        }
        break;
    }
    case 12: { // 矩阵列下落
        for (SkyPetal& column : state.petals) {
            column.y += column.vy;
            const float tail = column.size * 15.0f;
            if (column.y - tail > height) {
                column.y = -static_cast<float>(rand() % 200);
                column.vy = 10.0f + static_cast<float>(rand() % 40) / 2.0f;
                column.phase = static_cast<float>(rand() % 1000);
            }
        }
        break;
    }
    case 13: // 星云旋转：靠 time
        updateStars();
        break;
    case 16: { // 薄荷：星星闪烁 + 偶尔光带
        updateStars();
        if (rand() % 100 < 4) {
            const int fw = std::max(1, width);
            SkyMeteor meteor;
            meteor.x = -static_cast<float>(rand() % (fw / 2 + 1));
            meteor.y = static_cast<float>(rand() % std::max(1, height));
            meteor.vx = 6.0f + static_cast<float>(rand() % 30) / 10.0f;
            meteor.vy = 1.5f + static_cast<float>(rand() % 15) / 10.0f;
            state.meteors.push_back(meteor);
        }
        for (SkyMeteor& meteor : state.meteors) {
            meteor.x += meteor.vx;
            meteor.y += meteor.vy;
        }
        state.meteors.erase(
            std::remove_if(state.meteors.begin(), state.meteors.end(),
                           [width, height](const SkyMeteor& meteor) {
                               return meteor.x > width + 260 ||
                                      meteor.y > height + 260;
                           }),
            state.meteors.end());
        break;
    }
    case 19: { // 深海鲸落：海雪缓慢下沉
        for (SkyPetal& snow : state.petals) {
            snow.y += snow.vy;
            snow.x += std::sin((snow.y + snow.phase) * 0.02f) * 0.4f;
            if (snow.y > height + 10.0f) {
                snow.y = -10.0f;
                snow.x = static_cast<float>(rand() % std::max(1, width));
            }
        }
        break;
    }
    case 21: // 星塔夜航
        updateStars();
        break;
    case 22: { // 暮色天桥：云带漂移
        for (SkyPetal& cloud : state.petals) {
            cloud.x += cloud.vx * 2.0f;
            if (cloud.x > width + cloud.size * 2.0f) {
                cloud.x = -cloud.size * 2.0f;
            }
        }
        break;
    }
    case 23: { // 夏日云路：积雨云漂移
        for (SkyPetal& cloud : state.petals) {
            cloud.x += cloud.vx * 2.0f;
            if (cloud.x > width + cloud.size * 2.0f) {
                cloud.x = -cloud.size * 2.0f;
            }
        }
        break;
    }
    case 29: { // 極：云雾漂移
        for (SkyPetal& cloud : state.petals) {
            cloud.x += cloud.vx * 2.0f;
            if (cloud.x > width + cloud.size * 2.0f) {
                cloud.x = -cloud.size * 2.0f;
            }
        }
        break;
    }
    case 20: { // 青空物语：云漂移 + 飞鸟横越 + 晚霞拖尾
        for (SkyPetal& cloud : state.petals) {
            if (cloud.shade == 2) {
                cloud.x += cloud.vx;
                cloud.y += cloud.vy +
                           std::sin((state.time + cloud.phase) * 0.03f) * 0.12f;
                const float margin = cloud.size * 4.0f + 8.0f;
                if (cloud.x > width + margin) {
                    cloud.x = -margin;
                    cloud.y = static_cast<float>(height) * 0.16f +
                              static_cast<float>(rand() %
                                                 std::max(1, height / 2));
                }
                if (cloud.y < -10.0f) {
                    cloud.y = static_cast<float>(height) * 0.6f;
                }
                continue;
            }
            cloud.x += cloud.vx;
            const float margin = cloud.size * 2.0f + 20.0f;
            if (cloud.x > width + margin) {
                cloud.x = -margin;
            }
        }
        if (rand() % 100 < 4) {
            const int fw = std::max(1, width);
            SkyMeteor meteor;
            meteor.x = static_cast<float>(rand() % (fw / 2 + 1));
            meteor.y = static_cast<float>(rand() % std::max(1, height / 3));
            meteor.vx = 5.0f + static_cast<float>(rand() % 30) / 10.0f;
            meteor.vy = 2.0f + static_cast<float>(rand() % 15) / 10.0f;
            state.meteors.push_back(meteor);
        }
        for (SkyMeteor& meteor : state.meteors) {
            meteor.x += meteor.vx;
            meteor.y += meteor.vy;
        }
        state.meteors.erase(
            std::remove_if(state.meteors.begin(), state.meteors.end(),
                           [width, height](const SkyMeteor& meteor) {
                               return meteor.x > width + 320 ||
                                      meteor.y > height + 320;
                           }),
            state.meteors.end());
        break;
    }
    default:
        break;
    }
}

void DrawSky(HDC dc, const SkyState& state, int width, int height) {
    switch (g_theme) {
    case 2:
        DrawStarfield(dc, state, width, height);
        break;
    case 3:
        DrawBlackHole(dc, state, width, height);
        break;
    case 4:
        DrawSakura(dc, state, width, height);
        break;
    case 5:
        DrawEarth(dc, state, width, height);
        break;
    case 6:
        DrawAurora(dc, state, width, height);
        break;
    case 7:
        DrawRain(dc, state, width, height);
        break;
    case 8:
        DrawFireflies(dc, state, width, height);
        break;
    case 9:
        DrawMaple(dc, state, width, height);
        break;
    case 10:
        DrawBubbles(dc, state, width, height);
        break;
    case 11:
        DrawCyber(dc, state, width, height);
        break;
    case 12:
        DrawMatrix(dc, state, width, height);
        break;
    case 13:
        DrawNebula(dc, state, width, height);
        break;
    case 14:
        DrawCream(dc, state, width, height);
        break;
    case 15:
        DrawMoon(dc, state, width, height);
        break;
    case 16:
        DrawMint(dc, state, width, height);
        break;
    case 17:
        DrawMoonBack(dc, state, width, height);
        break;
    case 18:
        DrawCircuit(dc, state, width, height);
        break;
    case 19:
        DrawWhale(dc, state, width, height);
        break;
    case 21:
        DrawStarTower(dc, state, width, height);
        break;
    case 22:
        DrawDuskBridge(dc, state, width, height);
        break;
    case 23:
        DrawSummerRoad(dc, state, width, height);
        break;
    case 24:
        DrawNightBoat(dc, state, width, height);
        break;
    case 25:
        DrawForestBridge(dc, state, width, height);
        break;
    case 26:
        DrawDesertNight(dc, state, width, height);
        break;
    case 27:
        DrawNeonCity(dc, state, width, height);
        break;
    case 28:
        DrawWindTurbine(dc, state, width, height);
        break;
    case 29:
        DrawChampion(dc, state, width, height);
        break;
    case 20:
        DrawAnimeSky(dc, state, width, height);
        break;
    default:
        DrawStarfield(dc, state, width, height);
        break;
    }
}