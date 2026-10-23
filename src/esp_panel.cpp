#include "esp_launcher.h"
#include "ui.h"
#include "version.h"

#include <cmath>
#include <cstdio>
#include <unordered_map>

#include <dwmapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;
using ui::fillRR;
using ui::fonts;
using ui::strokeRR;
using ui::text;

namespace {

constexpr float W = 740, H = 520;
constexpr float SW = 188;
constexpr float CX = SW + 28, CW = 496;
constexpr float kCorner = 8;
constexpr UINT_PTR kRefreshTimer = 1, kAnimTimer = 2;

namespace th {
const Color bg(255, 14, 14, 20), bgLow(255, 10, 10, 15), side(255, 9, 9, 13);
const Color surface(255, 21, 21, 30), surfaceHot(255, 28, 28, 40), track(255, 44, 44, 60);
const Color border(255, 36, 36, 50), borderHot(255, 58, 58, 80);
const Color text(255, 238, 238, 246), dim(255, 140, 140, 162), faint(255, 84, 84, 106);
const Color a1(255, 124, 92, 255), a2(255, 76, 142, 255), accentText(255, 168, 148, 255);
const Color ok(255, 74, 222, 128), red(255, 244, 87, 107), red2(255, 214, 56, 96), white(255, 255, 255, 255);
}

enum Id : int {
    None = -1, BtnClose, BtnMin, DispPrev, DispNext, BtnFullscreen, BtnIdentify, BtnStart,
};

enum : int { KeyHover = 10000, KeyOn = 20000, KeyRun = 31001 };

struct Hit {
    int id;
    RectF r;
};

struct Anim {
    float v, t;
};

struct Panel {
    HWND hwnd = nullptr;
    float scale = 1.0f;
    int hot = None, down = None;
    bool tracking = false, animTimer = false;
    EspSettings* s = nullptr;
    PanelHooks hooks;
    std::vector<DisplayInfo> displays;
    std::vector<Hit> hits;
    std::unordered_map<int, Anim> anims;
    bool shownRun = false;
    PanelInfo shown;
} p;

void redraw() { InvalidateRect(p.hwnd, nullptr, FALSE); }
bool running() { return p.hooks.running && p.hooks.running(); }


float animTo(int key, float target) {
    auto it = p.anims.find(key);
    if (it == p.anims.end()) {
        p.anims[key] = {target, target};
        return target;
    }
    it->second.t = target;
    return it->second.v;
}

bool anyMoving() {
    for (const auto& kv : p.anims)
        if (kv.second.v != kv.second.t) return true;
    return false;
}

bool stepAnims() {
    bool moving = false;
    for (auto& kv : p.anims) {
        Anim& a = kv.second;
        const float d = a.t - a.v;
        if (std::fabs(d) < 0.01f) {
            a.v = a.t;
            continue;
        }
        a.v += d * 0.34f;
        moving = true;
    }
    return moving;
}

float hover(int id) { return animTo(KeyHover + id, p.hot == id ? 1.0f : 0.0f); }
bool pressed(int id) { return p.down == id && p.hot == id; }


Color lerp(Color a, Color b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    auto mix = [&](int x, int y) { return static_cast<BYTE>(x + (y - x) * t + 0.5f); };
    return Color(mix(a.GetA(), b.GetA()), mix(a.GetR(), b.GetR()), mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
}

Color withAlpha(Color c, int a) { return Color(static_cast<BYTE>(a), c.GetR(), c.GetG(), c.GetB()); }

void fillGrad(Graphics& g, RectF r, Color c1, Color c2, float rad, LinearGradientMode mode = LinearGradientModeHorizontal) {
    LinearGradientBrush b(RectF(r.X - 0.5f, r.Y - 0.5f, r.Width + 1, r.Height + 1), c1, c2, mode);
    GraphicsPath path;
    ui::roundedRect(path, r, rad);
    g.FillPath(&b, &path);
}

void glow(Graphics& g, RectF r, Color c, float rad, int alpha) {
    for (int i = 9; i >= 1; --i) {
        const float e = i * 1.7f;
        SolidBrush b(withAlpha(c, alpha));
        fillRR(g, b, RectF(r.X - e, r.Y - e + 5, r.Width + e * 2, r.Height + e * 2), rad + e);
    }
}

float textW(Graphics& g, const std::string& s, const Font& f) {
    RectF out;
    const std::wstring w = ui::widen(s);
    g.MeasureString(w.c_str(), -1, &f, PointF(0, 0), &out);
    return out.Width;
}

void label(Graphics& g, const std::string& s, float x, float y, float w = 200) {
    text(g, s, *fonts.micro, RectF(x, y, w, 14), th::faint, StringAlignmentNear);
}

void dot(Graphics& g, float cx, float cy, float r, Color c, bool halo) {
    if (halo) {
        SolidBrush h(withAlpha(c, 46));
        g.FillEllipse(&h, cx - r * 2.2f, cy - r * 2.2f, r * 4.4f, r * 4.4f);
    }
    SolidBrush b(c);
    g.FillEllipse(&b, cx - r, cy - r, r * 2, r * 2);
}

void card(Graphics& g, RectF r, float hot, Color border) {
    SolidBrush b(lerp(th::surface, th::surfaceHot, hot));
    fillRR(g, b, r, 10);
    strokeRR(g, border, r, 10);
}

void switchKnob(Graphics& g, RectF r, float k) {
    SolidBrush trackB(lerp(th::track, th::a1, k));
    fillRR(g, trackB, r, r.Height / 2);
    if (k < 0.5f) strokeRR(g, withAlpha(th::borderHot, static_cast<int>(255 * (1 - 2 * k))), r, r.Height / 2);
    const float d = r.Height - 6;
    SolidBrush knob(lerp(th::dim, th::white, k));
    g.FillEllipse(&knob, r.X + 3 + (r.Width - 6 - d) * k, r.Y + 3, d, d);
}

enum class Icon { Sliders, Crosshair };

void drawIcon(Graphics& g, Icon icon, float x, float y, Color c) {
    Pen pen(c, 1.6f);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush fill(c);
    switch (icon) {
        case Icon::Sliders:
            g.DrawLine(&pen, x, y + 4, x + 18, y + 4);
            g.DrawLine(&pen, x, y + 9, x + 18, y + 9);
            g.DrawLine(&pen, x, y + 14, x + 18, y + 14);
            for (const auto& k : {PointF(x + 12, y + 4), PointF(x + 5, y + 9), PointF(x + 13, y + 14)}) {
                SolidBrush hole(th::side);
                g.FillEllipse(&hole, k.X - 3, k.Y - 3, 6.0f, 6.0f);
                g.DrawEllipse(&pen, k.X - 2.6f, k.Y - 2.6f, 5.2f, 5.2f);
            }
            break;
        case Icon::Crosshair:
            g.DrawEllipse(&pen, x + 3.5f, y + 3.5f, 11.0f, 11.0f);
            g.DrawLine(&pen, x + 9, y, x + 9, y + 5);
            g.DrawLine(&pen, x + 9, y + 13, x + 9, y + 18);
            g.DrawLine(&pen, x, y + 9, x + 5, y + 9);
            g.DrawLine(&pen, x + 13, y + 9, x + 18, y + 9);
            break;
    }
}


void sideItem(Graphics& g, const std::string& name, Icon icon, float y) {
    const RectF r(12, y, SW - 24, 40);
    SolidBrush bg(Color(15, 255, 255, 255));
    fillRR(g, bg, r, 10);
    drawIcon(g, icon, r.X + 15, r.Y + 11, th::accentText);
    text(g, name, *fonts.semi, RectF(r.X + 46, r.Y, r.Width - 50, r.Height), th::white, StringAlignmentNear);
    fillGrad(g, RectF(0, y + 10, 3.5f, 20), th::a1, th::a2, 1.75f, LinearGradientModeVertical);
}

void winButton(Graphics& g, int id, RectF r, bool close) {
    const float h = hover(id);
    if (h > 0.01f) {
        SolidBrush b(close ? withAlpha(th::red2, static_cast<int>(255 * h)) : Color(static_cast<BYTE>(22 * h), 255, 255, 255));
        fillRR(g, b, r, 8);
    }
    Pen pen(close && h > 0.5f ? th::white : lerp(th::dim, th::text, h), 1.4f);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    const float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
    if (close) {
        g.DrawLine(&pen, cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f);
        g.DrawLine(&pen, cx + 4.5f, cy - 4.5f, cx - 4.5f, cy + 4.5f);
    } else {
        g.DrawLine(&pen, cx - 5, cy, cx + 5, cy);
    }
    p.hits.push_back({id, r});
}

void chip(Graphics& g, int id, const std::string& s, RectF r, bool on, bool enabled = true, const Font* f = nullptr) {
    const float h = enabled ? hover(id) : 0.0f;
    const float press = enabled && pressed(id) ? 1.0f : 0.0f;
    if (on) {
        const float dimK = enabled ? 0.0f : 0.55f;
        const Color c1 = lerp(lerp(th::a1, th::white, h * 0.1f), th::surface, dimK), c2 = lerp(lerp(th::a2, th::white, h * 0.1f), th::surface, dimK);
        fillGrad(g, r, lerp(c1, th::bgLow, press * 0.25f), lerp(c2, th::bgLow, press * 0.25f), 8);
    } else {
        SolidBrush b(lerp(th::surface, th::surfaceHot, h));
        fillRR(g, b, r, 8);
        strokeRR(g, lerp(th::border, th::borderHot, h), r, 8);
    }
    text(g, s, f ? *f : *fonts.semi, r, on ? (enabled ? th::white : th::dim) : enabled ? lerp(th::dim, th::text, h) : th::faint);
    if (enabled) p.hits.push_back({id, r});
}

std::string displayLabel() {
    const DisplayInfo* d = pickDisplay(p.displays, *p.s);
    if (!d) return "no display";
    char buf[96];
    std::snprintf(buf, sizeof buf, "Display %d · %ldx%ld%s", d->number, d->width(), d->height(), d->primary ? " · main" : "");
    return buf;
}


void setupPage(Graphics& g, bool run) {
    label(g, "DISPLAY", CX, 96);
    const RectF dc(CX, 114, CW, 52);
    card(g, dc, 0, th::border);
    {
        const bool fs = p.s->fullscreen;
        chip(g, DispPrev, "‹", RectF(CX + 12, dc.Y + 13, 26, 26), false, !run && fs, fonts.btn.get());
        text(g, fs ? displayLabel() : "Own window on this PC", *fonts.semi, RectF(CX + 42, dc.Y, 176, dc.Height), fs ? th::accentText : th::dim);
        chip(g, DispNext, "›", RectF(CX + 222, dc.Y + 13, 26, 26), false, !run && fs, fonts.btn.get());

        const RectF fsHit(CX + 262, dc.Y + 8, 112, 36);
        const float h = run ? 0.0f : hover(BtnFullscreen), k = animTo(KeyOn + BtnFullscreen, fs ? 1.0f : 0.0f);
        text(g, "Fullscreen", *fonts.caption, RectF(fsHit.X, dc.Y, 66, dc.Height), run ? th::faint : lerp(th::dim, th::text, h), StringAlignmentNear);
        switchKnob(g, RectF(CX + 334, dc.Y + 15, 38, 22), k);
        if (!run) p.hits.push_back({BtnFullscreen, fsHit});

        chip(g, BtnIdentify, "Identify", RectF(CX + CW - 12 - 88, dc.Y + 13, 88, 26), false);
    }

    const RectF sb(CX, 182, CW, 48);
    const float rk = animTo(KeyRun, run ? 1.0f : 0.0f), h = hover(BtnStart), press = pressed(BtnStart) ? 1.0f : 0.0f;
    glow(g, sb, lerp(th::a1, th::red, rk), 12, static_cast<int>(3 + 3 * h));
    const Color c1 = lerp(lerp(th::a1, th::red, rk), th::white, h * 0.1f), c2 = lerp(lerp(th::a2, th::red2, rk), th::white, h * 0.1f);
    fillGrad(g, sb, lerp(c1, th::bgLow, press * 0.25f), lerp(c2, th::bgLow, press * 0.25f), 12);
    {
        const std::string s = run ? "Stop" : "Start";
        const float tw = textW(g, s, *fonts.semi), cx = sb.X + sb.Width / 2, cy = sb.Y + sb.Height / 2;
        text(g, s, *fonts.semi, RectF(cx - tw / 2 + 10, sb.Y, tw + 4, sb.Height), th::white, StringAlignmentNear);
        SolidBrush w(th::white);
        const float ix = cx - tw / 2 - 12;
        if (run) {
            fillRR(g, w, RectF(ix - 5, cy - 5, 10, 10), 2);
        } else {
            const PointF tri[3] = {PointF(ix - 4, cy - 6), PointF(ix - 4, cy + 6), PointF(ix + 6, cy)};
            g.FillPolygon(&w, tri, 3);
        }
        p.hits.push_back({BtnStart, sb});
    }

    const PanelInfo in = run && p.hooks.info ? p.hooks.info() : PanelInfo{};
    p.shownRun = run;
    p.shown = in;
    char pid[32], speed[48];
    std::snprintf(pid, sizeof pid, "%lu", in.pid);
    std::snprintf(speed, sizeof speed, "%.1f MB/s", in.mbps);
    const std::pair<const char*, std::string> rows[] = {
        {"World PID", in.pid ? pid : "-"},
        {"Server", in.server.empty() ? "-" : in.server},
        {"DMA speed", in.mbps > 0 ? speed : "-"},
    };
    constexpr int kRows = 3;
    const RectF ib(CX, 250, CW, kRows * 44 + 8);
    card(g, ib, 0, th::border);
    for (int i = 0; i < kRows; ++i) {
        const float y = ib.Y + 4 + i * 44;
        if (i) {
            Pen div(th::border, 1.0f);
            g.DrawLine(&div, ib.X + 14, y, ib.X + ib.Width - 14, y);
        }
        text(g, rows[i].first, *fonts.caption, RectF(ib.X + 18, y, 150, 44), th::dim, StringAlignmentNear);
        text(g, rows[i].second, *fonts.semi, RectF(ib.X + 170, y, ib.Width - 188, 44), rows[i].second == "-" ? th::faint : th::text,
             StringAlignmentFar, StringAlignmentCenter, true);
    }
}

void paint(HDC hdc) {
    RECT rc;
    GetClientRect(p.hwnd, &rc);
    Bitmap bmp(rc.right, rc.bottom, PixelFormat32bppPARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.ScaleTransform(p.scale, p.scale);
    p.hits.clear();

    const bool run = running();

    {
        LinearGradientBrush bg(RectF(0, 0, W, H), th::bg, th::bgLow, LinearGradientModeVertical);
        g.FillRectangle(&bg, 0.0f, 0.0f, W, H);

        GraphicsPath bloom;
        bloom.AddEllipse(RectF(SW - 40, -190, 560, 340));
        PathGradientBrush pg(&bloom);
        pg.SetCenterColor(Color(36, 124, 92, 255));
        const Color edge(0, 124, 92, 255);
        int n = 1;
        pg.SetSurroundColors(&edge, &n);
        g.SetClip(RectF(SW, 0, W - SW, H));
        g.FillPath(&pg, &bloom);
        g.ResetClip();

        SolidBrush side(th::side);
        g.FillRectangle(&side, 0.0f, 0.0f, SW, H);
        Pen div(th::border, 1.0f);
        g.DrawLine(&div, SW, 0.0f, SW, H);
    }
    strokeRR(g, th::border, RectF(0.5f, 0.5f, W - 1, H - 1), kCorner);

    {
        fillGrad(g, RectF(20, 22, 34, 34), th::a1, th::a2, 10, LinearGradientModeForwardDiagonal);
        drawIcon(g, Icon::Crosshair, 28, 30, th::white);
        text(g, "ICKZA", *fonts.semi, RectF(64, 22, SW - 72, 18), th::text, StringAlignmentNear);
        text(g, "DayZ · v" ICKZA_VERSION, *fonts.caption, RectF(64, 40, SW - 72, 16), th::dim, StringAlignmentNear);

        sideItem(g, "Setup", Icon::Sliders, 92);

        const RectF chipR(16, H - 72, SW - 32, 34);
        SolidBrush cb(th::surface);
        fillRR(g, cb, chipR, 10);
        strokeRR(g, th::border, chipR, 10);
        dot(g, chipR.X + 18, chipR.Y + 17, 3.5f, run ? th::ok : th::faint, run);
        text(g, run ? "Running" : "Idle", *fonts.caption, RectF(chipR.X + 34, chipR.Y, chipR.Width - 40, chipR.Height),
             run ? th::text : th::dim, StringAlignmentNear);
        text(g, "END quits", *fonts.micro, RectF(0, H - 30, SW, 14), th::faint);
    }

    {
        text(g, "Setup", *fonts.heading, RectF(CX, 20, CW - 100, 30), th::text, StringAlignmentNear);
        text(g, "Pick the display, then start.", *fonts.caption, RectF(CX, 52, CW - 100, 18), th::dim, StringAlignmentNear);
        winButton(g, BtnMin, RectF(W - 84, 14, 32, 28), false);
        winButton(g, BtnClose, RectF(W - 48, 14, 32, 28), true);
    }

    setupPage(g, run);

    Graphics screen(hdc);
    screen.DrawImage(&bmp, 0, 0);

    if (anyMoving() && !p.animTimer) {
        SetTimer(p.hwnd, kAnimTimer, 15, nullptr);
        p.animTimer = true;
    }
}

int hitTest(float x, float y) {
    for (auto it = p.hits.rbegin(); it != p.hits.rend(); ++it)
        if (it->r.Contains(x, y)) return it->id;
    return None;
}

void cycleDisplay(int dir) {
    if (p.displays.empty()) return;
    const DisplayInfo* cur = pickDisplay(p.displays, *p.s);
    const int n = static_cast<int>(p.displays.size());
    const int i = ((cur ? cur->number - 1 : 0) + dir + n) % n;
    const DisplayInfo& d = p.displays[static_cast<size_t>(i)];
    p.s->displayDevice = d.device;
    p.s->displayW = d.width();
    p.s->displayH = d.height();
}

void activate(int id) {
    const bool run = running();
    switch (id) {
        case BtnClose: PostMessage(p.hwnd, WM_CLOSE, 0, 0); break;
        case BtnMin: ShowWindow(p.hwnd, SW_MINIMIZE); break;
        case DispPrev:
        case DispNext:
            if (!run) cycleDisplay(id == DispNext ? 1 : -1);
            break;
        case BtnFullscreen:
            if (!run) p.s->fullscreen = !p.s->fullscreen;
            break;
        case BtnIdentify: identifyDisplays(p.displays); break;
        case BtnStart:
            if (run) p.hooks.stop();
            else {
                p.s->dma = true;
                p.s->dzRadarScreen = true;
                p.hooks.start();
            }
            break;
        default: break;
    }
    saveSettings(*p.s);
    redraw();
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto logical = [&](LPARAM l, float& x, float& y) {
        x = static_cast<float>(static_cast<short>(LOWORD(l))) / p.scale;
        y = static_cast<float>(static_cast<short>(HIWORD(l))) / p.scale;
    };
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            paint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kAnimTimer) {
                if (!stepAnims()) {
                    KillTimer(hwnd, kAnimTimer);
                    p.animTimer = false;
                }
                if (!IsIconic(hwnd)) redraw();
            } else if (wp == kRefreshTimer && !IsIconic(hwnd)) {
                const bool run = running();
                const PanelInfo in = run && p.hooks.info ? p.hooks.info() : PanelInfo{};
                const auto mb = [](double v) { return int(v * 10 + 0.5); };
                if (run != p.shownRun || in.pid != p.shown.pid || in.server != p.shown.server || mb(in.mbps) != mb(p.shown.mbps))
                    redraw();
            }
            return 0;
        case WM_NCHITTEST: {
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            ScreenToClient(hwnd, &pt);
            const float x = pt.x / p.scale, y = pt.y / p.scale;
            if ((y < 78 || x < SW) && hitTest(x, y) == None) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_MOUSEMOVE: {
            float x, y;
            logical(lp, x, y);
            const int h = hitTest(x, y);
            if (!p.tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                p.tracking = true;
            }
            if (h != p.hot) {
                p.hot = h;
                redraw();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            p.tracking = false;
            p.hot = None;
            redraw();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && p.hot != None) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            float x, y;
            logical(lp, x, y);
            p.down = hitTest(x, y);
            redraw();
            return 0;
        }
        case WM_LBUTTONUP: {
            float x, y;
            logical(lp, x, y);
            const int up = hitTest(x, y), was = p.down;
            p.down = None;
            if (up != None && up == was) activate(up);
            else redraw();
            return 0;
        }
        case WM_DISPLAYCHANGE:
            p.displays = listDisplays();
            redraw();
            break;
        case WM_DESTROY:
            KillTimer(hwnd, kRefreshTimer);
            KillTimer(hwnd, kAnimTimer);
            if (running()) p.hooks.stop();
            p.hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}


HWND createPanel(EspSettings& s, PanelHooks hooks) {
    p.s = &s;
    p.hooks = std::move(hooks);
    p.displays = listDisplays();
    p.scale = GetDpiForSystem() / 96.0f;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ickza_panel";
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = static_cast<int>(W * p.scale), h = static_cast<int>(H * p.scale);
    const int x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 2;
    p.hwnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Ickza", WS_POPUP | WS_MINIMIZEBOX, x, y, w, h, nullptr, nullptr,
                             wc.hInstance, nullptr);
    if (!p.hwnd) return nullptr;
    const DWORD round = 2;
    if (FAILED(DwmSetWindowAttribute(p.hwnd, 33 , &round, sizeof round))) {
        const int r = static_cast<int>(kCorner * 2 * p.scale);
        SetWindowRgn(p.hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, r, r), TRUE);
    }
    SetTimer(p.hwnd, kRefreshTimer, 250, nullptr);
    ShowWindow(p.hwnd, SW_SHOW);
    return p.hwnd;
}
