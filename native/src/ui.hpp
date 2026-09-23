// ui.hpp — a tiny immediate-mode widget layer over GDI.
//
// Not a UI framework. Just enough to have real buttons with hover and press
// states, sliders you can drag, and text that lays out, without retained
// widget objects or a callback graph. Every widget is a function call that
// draws itself and returns whether it was used this frame.
//
// The whole thing is ~200 lines because the workbench needs about six widgets
// and none of them need to be general.

#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

namespace ui {

// Raised across the board for legibility. The old palette put body text at
// 0x7a8b96 on 0x0b0e11 — about 5:1 — and the "faint" tier at 0x44535e, which is
// roughly 2.5:1 and below any reasonable reading threshold. Tooltips and hints
// were written in that tier, so the explanatory text was the hardest thing on
// screen to read. Panels are also separated by lightness now rather than by a
// hairline alone.
struct Theme {
    COLORREF bg      = RGB(0x0e,0x12,0x17);
    COLORREF panel   = RGB(0x18,0x1f,0x27);
    COLORREF sunken  = RGB(0x0a,0x0e,0x12);
    COLORREF line    = RGB(0x2e,0x3a,0x45);
    COLORREF ink     = RGB(0xf0,0xf5,0xf8);   // body text, ~15:1
    COLORREF dim     = RGB(0xa8,0xb8,0xc4);   // secondary, ~8:1
    COLORREF faint   = RGB(0x78,0x8a,0x98);   // hints, ~4.6:1 — still readable
    COLORREF acc     = RGB(0x6f,0xe3,0xd4);
    COLORREF amber   = RGB(0xff,0xcf,0x66);
    COLORREF good    = RGB(0x8d,0xe8,0xa1);
    COLORREF bad     = RGB(0xff,0x6f,0x6a);
    COLORREF viol    = RGB(0xb0,0x92,0xff);
    COLORREF hover   = RGB(0x25,0x30,0x3a);
    COLORREF active  = RGB(0x30,0x40,0x4c);
};

// Per-frame input, owned by the app and handed to every widget call.
struct Ctx {
    HDC   dc = nullptr;
    POINT mouse{};
    bool  down = false;        // button currently held
    bool  clicked = false;     // a click was released this frame, not yet consumed
    HFONT font = nullptr, bold = nullptr;
    Theme t;
    int   hot = 0;             // id of the widget under the cursor (for cursor shape)
    RECT inputClip{0,0,100000,100000};
    bool audit = false;
    int overflow = 0;
    std::vector<RECT> widgets;
    POINT pressPoint{};

    // Tooltip for whatever the cursor is over THIS frame. Immediate-mode: any
    // widget can claim it while drawing, and the window draws it last so it
    // lands on top of everything instead of under the next pane.
    std::string tipText;
    RECT        tipRect{};
};

inline void fill(Ctx& c, RECT r, COLORREF col) {
    HBRUSH b = CreateSolidBrush(col); FillRect(c.dc, &r, b); DeleteObject(b);
}
inline void frame(Ctx& c, RECT r, COLORREF col) {
    HBRUSH b = CreateSolidBrush(col); FrameRect(c.dc, &r, b); DeleteObject(b);
}
inline void hline(Ctx& c, int x0, int x1, int y, COLORREF col) {
    RECT r{x0,y,x1,y+1}; fill(c, r, col);
}
inline void vline(Ctx& c, int x, int y0, int y1, COLORREF col) {
    RECT r{x,y0,x+1,y1}; fill(c, r, col);
}
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8,0,s.c_str(),int(s.size()),nullptr,0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8,0,s.c_str(),int(s.size()),w.data(),n);
    return w;
}
inline SIZE measure(Ctx& c, const std::string& s, HFONT f) {
    SelectObject(c.dc, f ? f : c.font);
    auto w = widen(s); SIZE sz{};
    GetTextExtentPoint32W(c.dc, w.c_str(), int(w.size()), &sz);
    return sz;
}
inline int text(Ctx& c, int x, int y, const std::string& s, COLORREF col, HFONT f = nullptr) {
    SelectObject(c.dc, f ? f : c.font);
    SetTextColor(c.dc, col); SetBkMode(c.dc, TRANSPARENT);
    auto w = widen(s); TextOutW(c.dc, x, y, w.c_str(), int(w.size()));
    SIZE sz{}; GetTextExtentPoint32W(c.dc, w.c_str(), int(w.size()), &sz);
    return sz.cy;
}
inline int textRight(Ctx& c, int xr, int y, const std::string& s, COLORREF col, HFONT f = nullptr) {
    const SIZE sz = measure(c, s, f);
    return text(c, xr - sz.cx, y, s, col, f);
}
// word-wrapped paragraph; returns height used
// draw=false measures without painting, so a box can be sized to its text
// before anything is put on screen. Same wrapping either way, by construction.
inline int para(Ctx& c, int x, int y, int width, const std::string& s,
                COLORREF col, HFONT f = nullptr, bool draw = true) {
    SelectObject(c.dc, f ? f : c.font);
    SetTextColor(c.dc, col); SetBkMode(c.dc, TRANSPARENT);
    std::string line, word; int cy = y;
    auto flush = [&]{
        if (line.empty()) return;
        auto w = widen(line);
        if (draw) TextOutW(c.dc, x, cy, w.c_str(), int(w.size()));
        SIZE sz{}; GetTextExtentPoint32W(c.dc, w.c_str(), int(w.size()), &sz);
        cy += sz.cy + 1; line.clear();
    };
    for (std::size_t i = 0; i <= s.size(); ++i) {
        const char ch = i < s.size() ? s[i] : ' ';
        if (ch == ' ' || ch == '\n') {
            std::string cand = line.empty() ? word : line + " " + word;
            auto w = widen(cand); SIZE sz{};
            GetTextExtentPoint32W(c.dc, w.c_str(), int(w.size()), &sz);
            if (sz.cx > width && !line.empty()) { flush(); line = word; } else line = cand;
            word.clear();
            if (ch == '\n') flush();
        } else word.push_back(ch);
    }
    flush();
    return cy - y;
}

inline bool inside(const RECT& r, POINT p) { return PtInRect(&r, p) != 0; }
inline bool hit(Ctx& c, RECT r) { return inside(r, c.mouse) && inside(c.inputClip, c.mouse); }
inline void record(Ctx& c, RECT r) {
    if(!c.audit)return;
    RECT visible{};
    if(!IntersectRect(&visible,&r,&c.inputClip))return;
    if(r.left<c.inputClip.left || r.right>c.inputClip.right)++c.overflow;
    c.widgets.push_back(visible);
}

inline void rounded(Ctx& c, RECT r, COLORREF bg, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    auto oldBrush = SelectObject(c.dc, brush); auto oldPen = SelectObject(c.dc, pen);
    RoundRect(c.dc, r.left, r.top, r.right, r.bottom, 8, 8);
    SelectObject(c.dc, oldBrush); SelectObject(c.dc, oldPen);
    DeleteObject(brush); DeleteObject(pen);
}

// A button. Returns true on the frame it is clicked.
inline bool button(Ctx& c, RECT r, const std::string& label,
                   bool active = false, bool enabled = true, COLORREF tint = 0) {
    record(c,r);
    const bool over = enabled && hit(c, r);
    if (over) c.hot = 1;
    COLORREF bg = active ? c.t.active : (over ? c.t.hover : c.t.panel);
    rounded(c, r, bg, active || over ? c.t.acc : c.t.line);
    COLORREF fg = !enabled ? c.t.faint : (active ? c.t.acc : (over ? c.t.ink : c.t.dim));
    if (tint && enabled && !active) fg = tint;
    const SIZE sz = measure(c, label, c.font);
    const int save = SaveDC(c.dc);
    IntersectClipRect(c.dc, r.left + 5, r.top, r.right - 5, r.bottom);
    text(c, r.left + std::max(6L, (r.right-r.left-sz.cx)/2), r.top + (r.bottom-r.top-sz.cy)/2, label, fg);
    RestoreDC(c.dc, save);
    if (over && c.clicked && inside(r,c.pressPoint)) { c.clicked = false; return true; }
    return false;
}

// A horizontal slider. Writes through `v`; returns true while being dragged.
inline bool slider(Ctx& c, RECT r, float& v, float lo, float hi, bool& dragging) {
    record(c,r);
    const bool over = hit(c, r);
    if (over) c.hot = 1;
    if (over && c.down && inside(r,c.pressPoint)) dragging = true;
    if (!c.down) dragging = false;
    RECT track{ r.left, r.top + (r.bottom-r.top)/2 - 2, r.right, r.top + (r.bottom-r.top)/2 + 2 };
    fill(c, track, c.t.sunken);
    const float t = (hi > lo) ? std::clamp((v - lo) / (hi - lo), 0.f, 1.f) : 0.f;
    RECT filled = track; filled.right = track.left + int((track.right-track.left) * t);
    fill(c, filled, c.t.acc);
    const int kx = track.left + int((track.right-track.left) * t);
    RECT knob{ kx-3, r.top+2, kx+3, r.bottom-2 };
    fill(c, knob, over || dragging ? c.t.ink : c.t.acc);
    if (dragging) {
        const float nt = std::clamp(float(c.mouse.x - r.left) / float(std::max(1L, r.right - r.left)), 0.f, 1.f);
        v = lo + nt * (hi - lo);
        return true;
    }
    return false;
}

// Simple horizontal layout cursor for toolbars.
// Single-line text field. The workbench had no text input at all — every
// control was a button or a drag — so this is the whole widget: draw the box,
// draw the contents, draw a caret when focused. Key handling lives in the
// window proc, because only it knows about focus and hotkey suppression.
inline bool text_field(Ctx& c, RECT r, const std::string& txt, bool focused,
                       const std::string& placeholder = "") {
    fill(c, r, focused ? c.t.sunken : c.t.panel);
    frame(c, r, focused ? c.t.acc : c.t.line);
    const bool showPlaceholder = txt.empty() && !focused;
    text(c, r.left + 7, r.top + 5,
            showPlaceholder ? placeholder : txt,
            showPlaceholder ? c.t.faint : c.t.ink);
    if (focused) {
        // Caret sits after the last glyph, measured rather than guessed, so it
        // stays put in a proportional font.
        const SIZE sz = measure(c, txt, nullptr);
        RECT caret{ r.left + 7 + sz.cx + 1, r.top + 4,
                    r.left + 7 + sz.cx + 2, r.bottom - 4 };
        fill(c, caret, c.t.acc);
    }
    return inside(r, c.mouse) && c.clicked;
}

// Draw text, dropping trailing items that would collide with something already
// reserved on the right. Two separate right-hand readouts have now overrun
// left-hand text in this UI; measuring beats guessing a fixed inset.
inline void text_clipped(Ctx& c, int x, int y, const std::string& s, COLORREF col,
                         int rightLimit, const char* = "  ·  ", HFONT font = nullptr) {
    if(rightLimit<=x)return;
    const auto size=measure(c,s,font);
    SetTextColor(c.dc,col);SetBkMode(c.dc,TRANSPARENT);
    RECT r{x,y,rightLimit,y+size.cy};const auto wide=widen(s);
    DrawTextW(c.dc,wide.c_str(),int(wide.size()),&r,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
}

// Claim a tooltip and hand the rect straight back, so a widget call can be
// wrapped in place: button(c, tipped(c, bar.next(40), "..."), "label").
inline RECT tipped(Ctx& c, RECT r, const std::string& text);

// Claim the tooltip if the cursor is inside this rect. Called next to a widget
// rather than being a parameter on every widget signature, so existing call
// sites stay as they are.
inline void tip(Ctx& c, RECT r, const std::string& text) {
    if (text.empty() || !hit(c, r)) return;
    c.tipText = text;
    c.tipRect = r;
}

inline RECT tipped(Ctx& c, RECT r, const std::string& text) { tip(c, r, text); return r; }

// Draw the pending tooltip. Must be called LAST, after every pane, or panes
// drawn afterwards paint over it.
inline void draw_tip(Ctx& c, RECT client) {
    if (c.tipText.empty()) return;
    const int wrap = 320;
    const int th = para(c, 0, 0, wrap, c.tipText, c.t.ink, nullptr, false);

    // Measure the widest wrapped line rather than assuming the full wrap width,
    // so a three-word tip gets a three-word box.
    int tw = 0;
    { std::string line;
      std::size_t start = 0;
      for (std::size_t i = 0; i <= c.tipText.size(); ++i) {
          if (i == c.tipText.size() || c.tipText[i] == ' ') {
              const std::string cand = c.tipText.substr(start, i - start);
              const SIZE sz = measure(c, cand, nullptr);
              if (int(sz.cx) > tw) tw = std::min(int(sz.cx), wrap);
          }
      }
      tw = std::max(tw, std::min(int(measure(c, c.tipText, nullptr).cx), wrap));
    }

    // Prefer below-right of the cursor; flip when that would leave the window.
    int x = c.mouse.x + 16, y = c.mouse.y + 20;
    const int bw = tw + 20, bh = th + 14;
    if (x + bw > client.right  - 4) x = c.mouse.x - bw - 8;
    if (y + bh > client.bottom - 4) y = c.mouse.y - bh - 8;
    if (x < 4) x = 4;
    if (y < 4) y = 4;

    RECT box{ x, y, x + bw, y + bh };
    RECT shadow{ box.left + 2, box.top + 2, box.right + 2, box.bottom + 2 };
    fill(c, shadow, RGB(0x05,0x07,0x09));
    fill(c, box, RGB(0x1b,0x24,0x2b));
    frame(c, box, c.t.acc);
    para(c, box.left + 10, box.top + 7, wrap, c.tipText, c.t.ink);
}

// A small square toggle. Used for rule bits, where the label is one character
// and there may be eighteen of them, so a full-width button per switch would
// bury everything else in the pane.
inline bool chip(Ctx& c, RECT r, const std::string& label, bool on) {
    const bool over = inside(r, c.mouse);
    fill(c, r, on ? c.t.acc : (over ? c.t.hover : c.t.panel));
    frame(c, r, on ? c.t.acc : c.t.line);
    const SIZE sz = measure(c, label, nullptr);
    text(c, r.left + ((r.right - r.left) - sz.cx) / 2,
            r.top  + ((r.bottom - r.top) - sz.cy) / 2,
            label, on ? c.t.bg : c.t.dim);
    return over && c.clicked;
}

// A row of controls that knows where the row ENDS.
//
// Without a limit the bar happily walks past the right edge of its pane and
// draws button on top of button — which is exactly what happened the moment a
// 3D sim also gained training controls: "fit", "recentre" and "stamp" were
// painted over each other and two of them could not be read or reliably hit.
// Overlapping controls are worse than absent ones, because they still respond
// to clicks. So the bar wraps to a second line instead, and says how many lines
// it used.
struct Bar {
    int x, y, h, gap = 6;
    int x0 = 0;          // where a wrapped line starts
    int limit = 0;       // right edge; 0 = unbounded (old behaviour)
    int lineH = 0;       // vertical step when wrapping
    int lines = 1;

    RECT next(int w) {
        if (limit > 0 && x != x0 && x + w > limit) wrap();
        RECT r{ x, y, x + w, y + h };
        x += w + gap;
        return r;
    }
    void space(int w) { x += w; }
    void wrap() { x = x0; y += (lineH > 0 ? lineH : h + 8); ++lines; }
};

} // namespace ui
