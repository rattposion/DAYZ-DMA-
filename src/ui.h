#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>

#include <memory>
#include <string>

namespace ui {

const Gdiplus::Color kBg(255, 18, 16, 26);
const Gdiplus::Color kBorder(255, 44, 39, 66);
const Gdiplus::Color kPanel(255, 13, 11, 19);
const Gdiplus::Color kText(255, 232, 228, 245);
const Gdiplus::Color kDim(255, 130, 122, 160);
const Gdiplus::Color kAccent(255, 108, 79, 190);
const Gdiplus::Color kAccentHot(255, 128, 99, 214);
const Gdiplus::Color kAccentLo(255, 82, 60, 150);
const Gdiplus::Color kTarget(255, 142, 123, 255);
const Gdiplus::Color kTabIdle(255, 38, 34, 58);
const Gdiplus::Color kTabHot(255, 50, 45, 76);
const Gdiplus::Color kRed(255, 190, 70, 90);
const Gdiplus::Color kRedHot(255, 214, 90, 110);
const Gdiplus::Color kOk(255, 120, 210, 160);
const Gdiplus::Color kWhite(255, 255, 255, 255);

struct Fonts {
    std::unique_ptr<Gdiplus::FontFamily> sans, mono, semiFam;
    std::unique_ptr<Gdiplus::Font> title, sub, btn, body, caption, monoSmall;
    std::unique_ptr<Gdiplus::Font> semi, heading, micro;
};
extern Fonts fonts;

void init();
void shutdown();

std::wstring widen(const std::string& s);
void roundedRect(Gdiplus::GraphicsPath& p, Gdiplus::RectF r, float rad);
void fillRR(Gdiplus::Graphics& g, Gdiplus::Brush& b, Gdiplus::RectF r, float rad);
void strokeRR(Gdiplus::Graphics& g, Gdiplus::Color c, Gdiplus::RectF r, float rad, float w = 1.0f);
void text(Gdiplus::Graphics& g, const std::string& s, const Gdiplus::Font& f, Gdiplus::RectF r, Gdiplus::Color c,
          Gdiplus::StringAlignment h = Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignment v = Gdiplus::StringAlignmentCenter,
          bool ellipsis = false);
void button(Gdiplus::Graphics& g, const std::string& label, Gdiplus::RectF r, bool active, bool hot, bool pressed,
            Gdiplus::Color fillActive = kAccent, Gdiplus::Color fillHot = kAccentHot);

}
