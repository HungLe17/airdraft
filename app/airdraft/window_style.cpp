// SPDX-License-Identifier: GPL-2.0-only
#include "window_style.h"
#include <commctrl.h>
#include <string>

namespace airdraft::ui {
namespace {

LRESULT CALLBACK button_hover(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR)
{
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_QUERY, window};
        if (!TrackMouseEvent(&tracking) || !(tracking.dwFlags & TME_LEAVE)) {
            tracking = {sizeof(tracking), TME_LEAVE, window};
            TrackMouseEvent(&tracking);
            InvalidateRect(window, nullptr, FALSE);
        }
    } else if (message == WM_MOUSELEAVE) {
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, button_hover, id);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

}

int pixels(UINT dpi, int size)
{
    return MulDiv(size, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

Palette palette()
{
    HIGHCONTRASTW setting{sizeof(setting)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(setting), &setting, 0) &&
        (setting.dwFlags & HCF_HIGHCONTRASTON)) {
        const auto face = GetSysColor(COLOR_BTNFACE);
        const auto surface = GetSysColor(COLOR_WINDOW);
        const auto text = GetSysColor(COLOR_WINDOWTEXT);
        const auto button = GetSysColor(COLOR_BTNTEXT);
        return {face, surface, surface, text, text, text, face, face, button, button, button,
                GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHTTEXT),
                surface, surface, GetSysColor(COLOR_GRAYTEXT), true};
    }
    return {
        RGB(231, 237, 242), RGB(255, 255, 255), RGB(242, 246, 249), RGB(206, 218, 226),
        RGB(23, 42, 56), RGB(64, 86, 103),
        RGB(18, 37, 54), RGB(36, 60, 80), RGB(75, 101, 121), RGB(250, 252, 254), RGB(191, 211, 225),
        RGB(15, 103, 89), RGB(10, 80, 69), RGB(255, 255, 255),
        RGB(228, 243, 239), RGB(251, 236, 215), RGB(119, 138, 152), false};
}

HFONT font(UINT dpi, int height, int weight)
{
    return CreateFontW(-pixels(dpi, height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void fill(HDC dc, RECT area, COLORREF color)
{
    SetDCBrushColor(dc, color);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void panel(HDC dc, RECT area, int radius, COLORREF background, COLORREF outline)
{
    const auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, background);
    SetDCPenColor(dc, outline);
    if (radius) RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
    else Rectangle(dc, area.left, area.top, area.right, area.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
}

void enable_button_hover(HWND control)
{
    SetWindowSubclass(control, button_hover, 1, 0);
}

void draw_button(const DRAWITEMSTRUCT& item, UINT dpi, Button style, const Palette& colors)
{
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    POINT pointer{};
    const bool hovered = !disabled && !colors.contrast && GetCursorPos(&pointer) && WindowFromPoint(pointer) == item.hwndItem;
    const bool header = style == Button::header;
    const bool primary = style == Button::primary && !disabled;
    const auto background = primary ? (pressed || hovered ? colors.accent_pressed : colors.accent)
        : header ? (pressed || hovered ? colors.header_outline : colors.header_button)
        : pressed || hovered || disabled ? colors.inset : colors.surface;
    const auto outline = primary ? background : header ? colors.header_outline : colors.outline;
    const auto foreground = disabled ? colors.disabled : primary ? colors.on_accent : header ? colors.header_text : colors.text;
    const auto saved = SaveDC(item.hDC);
    fill(item.hDC, item.rcItem, header ? colors.header : colors.surface);
    panel(item.hDC, item.rcItem, pixels(dpi, colors.contrast ? 0 : 8), background, outline);
    const auto label_font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    if (label_font) SelectObject(item.hDC, label_font);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, foreground);
    std::wstring label(static_cast<size_t>(GetWindowTextLengthW(item.hwndItem)) + 1, L'\0');
    const auto length = GetWindowTextW(item.hwndItem, label.data(), static_cast<int>(label.size()));
    auto text = item.rcItem;
    InflateRect(&text, -pixels(dpi, 8), 0);
    UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    if (item.itemState & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
    DrawTextW(item.hDC, label.data(), length, &text, flags);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        auto focus = item.rcItem;
        InflateRect(&focus, -pixels(dpi, 4), -pixels(dpi, 4));
        DrawFocusRect(item.hDC, &focus);
    }
    RestoreDC(item.hDC, saved);
}

INT_PTR dialog_surface(HDC dc)
{
    const auto colors = palette();
    SetBkColor(dc, colors.surface);
    SetTextColor(dc, colors.text);
    SetDCBrushColor(dc, colors.surface);
    return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
}

}
