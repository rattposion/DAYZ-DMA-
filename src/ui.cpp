#include "ui.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace ui {

Fonts fonts;

void init() {
    fonts.sans = std::make_unique<FontFamily>(L"Segoe UI");
    fonts.mono = std::make_unique<FontFamily>(L"Consolas");
    fonts.title = std::make_unique<Font>(fonts.sans.get(), 22.0f, FontStyleBold, UnitPixel);
    fonts.sub = std::make_unique<Font>(fonts.sans.get(), 12.0f, FontStyleRegular, UnitPixel);
    fonts.btn = std::make_unique<Font>(fonts.sans.get(), 13.0f, FontStyleBold, UnitPixel);
    fonts.body = std::make_unique<Font>(fonts.sans.get(), 14.0f, FontStyleRegular, UnitPixel);
    fonts.caption = std::make_unique<Font>(fonts.sans.get(), 12.0f, FontStyleRegular, UnitPixel);
    fonts.monoSmall = std::make_unique<Font>(fonts.mono.get(), 12.0f, FontStyleRegular, UnitPixel);

    fonts.semiFam = std::make_unique<FontFamily>(L"Segoe UI Semibold");
    const bool semiOk = fonts.semiFam->IsAvailable();
    const FontFamily* sf = semiOk ? fonts.semiFam.get() : fonts.sans.get();
    const int style = semiOk ? FontStyleRegular : FontStyleBold;
    fonts.semi = std::make_unique<Font>(sf, 13.0f, style, UnitPixel);
    fonts.heading = std::make_unique<Font>(sf, 22.0f, style, UnitPixel);
    fonts.micro = std::make_unique<Font>(sf, 10.5f, style, UnitPixel);
}

void shutdown() { fonts = Fonts{}; }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(n - 1);
    return w;
}

void roundedRect(GraphicsPath& p, RectF r, float rad) {
    const float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

void fillRR(Graphics& g, Brush& b, RectF r, float rad) {
    GraphicsPath p;
    roundedRect(p, r, rad);
    g.FillPath(&b, &p);
}

void strokeRR(Graphics& g, Color c, RectF r, float rad, float w) {
    GraphicsPath p;
    roundedRect(p, r, rad);
    Pen pen(c, w);
    g.DrawPath(&pen, &p);
}

void text(Graphics& g, const std::string& s, const Font& f, RectF r, Color c, StringAlignment h, StringAlignment v, bool ellipsis) {
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    if (ellipsis) sf.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    const std::wstring w = widen(s);
    g.DrawString(w.c_str(), -1, &f, r, &sf, &b);
}

void button(Graphics& g, const std::string& label, RectF r, bool active, bool hot, bool pressed, Color fillActive, Color fillHot) {
    Color fill = active ? (pressed ? kAccentLo : hot ? fillHot : fillActive) : (hot ? kTabHot : kTabIdle);
    SolidBrush b(fill);
    fillRR(g, b, r, 8);
    if (!active) strokeRR(g, kBorder, r, 8);
    text(g, label, *fonts.btn, r, active ? kWhite : kDim);
}

}
