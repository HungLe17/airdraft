// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <windows.h>
#include <string_view>

namespace airdraft::ui {

struct Palette {
    COLORREF background, surface, inset, outline, text, secondary;
    COLORREF header, header_button, header_outline, header_text, header_secondary;
    COLORREF accent, accent_pressed, on_accent, status, caution, disabled;
    bool contrast;
};

enum class Button { secondary, primary, header };

int pixels(UINT dpi, int size);
Palette palette();
HFONT font(UINT dpi, int height, int weight = FW_NORMAL);
void fill(HDC dc, RECT area, COLORREF color);
void panel(HDC dc, RECT area, int radius, COLORREF background, COLORREF outline);
void enable_button_hover(HWND control);
void draw_button(const DRAWITEMSTRUCT& item, UINT dpi, Button style, const Palette& colors);
INT_PTR dialog_surface(HDC dc);

}
