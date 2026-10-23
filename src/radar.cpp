
#define NOMINMAX
#include "radar.h"

#include <windowsx.h>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include "imgui/backends/imgui_impl_win32.h"

#include <d3d11.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#include <cstdarg>

using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace radar {

namespace {
#ifndef ICKZA_NO_DIAG
void (*gLog)(const char*) = nullptr;
void rlog(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (gLog) gLog(b);
    else OutputDebugStringA(b);
}
#endif
}
#ifndef ICKZA_NO_DIAG
void setLog(void (*fn)(const char*)) { gLog = fn; }
#endif

namespace {

constexpr float kPi = 3.14159265f;

namespace th {
constexpr ImU32 Bg = IM_COL32(13, 15, 19, 255);
constexpr ImU32 Panel = IM_COL32(17, 19, 24, 246);
constexpr ImU32 Raised = IM_COL32(27, 31, 38, 255);
constexpr ImU32 Line = IM_COL32(42, 47, 58, 255);
constexpr ImU32 Text = IM_COL32(232, 236, 242, 255);
constexpr ImU32 Muted = IM_COL32(134, 142, 156, 255);
constexpr ImU32 Faint = IM_COL32(88, 95, 108, 255);
constexpr ImU32 Accent = IM_COL32(36, 226, 196, 255);
constexpr ImU32 Edge = IM_COL32(0, 0, 0, 200);
constexpr ImU32 BadgeFill = IM_COL32(15, 17, 21, 236);
constexpr COLORREF Survivor = RGB(255, 60, 60), Infected = RGB(255, 160, 40), Animal = RGB(210, 180, 110), Vehicle = RGB(170, 130, 255);
}

constexpr float kSide = 288.f;

enum { kGlyphPerson = 100, kGlyphPaw, kGlyphCar };

HWND gWnd = nullptr;
ComPtr<ID3D11Device> gDev;
ComPtr<ID3D11DeviceContext> gCtx;
ComPtr<IDXGISwapChain1> gSwap;
ComPtr<ID3D11RenderTargetView> gRtv;
int gW = 0, gH = 0;
bool gImguiReady = false;
ImFont* gFont = nullptr;
ImFont* gBold = nullptr;

float gPanX = 0, gPanZ = 0;
bool gRawWheel = false;
float gK = 1;
int gZoom = 800;
bool gDrag = false;
POINT gDragAt{};
bool gWantClose = false;
bool gSideOpen = true;

struct Decoded {
    std::mutex mx;
    bool done = false, ok = false;
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
};
struct Tex {
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
    std::shared_ptr<Decoded> pending;
    bool failed = false;
};
std::map<std::string, Tex> gTex;
ComPtr<ID3D11SamplerState> gMapSampler;

ImU32 col32(COLORREF c, int a = 255) { return IM_COL32(GetRValue(c), GetGValue(c), GetBValue(c), a); }
ImU32 withAlpha(ImU32 c, float a) {
    const int al = int(float((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.f, 1.f));
    return (c & ~IM_COL32_A_MASK) | (ImU32(al) << IM_COL32_A_SHIFT);
}
ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

void releaseRtv() { gRtv.Reset(); }

bool makeRtv() {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(gSwap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    return SUCCEEDED(gDev->CreateRenderTargetView(back.Get(), nullptr, &gRtv));
}

void decodeFile(const std::string& path, Decoded& d) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    std::vector<uint8_t> px;
    UINT iw = 0, ih = 0;
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> conv;
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, MAX_PATH);
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateDecoderFromFilename(wpath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&iw, &ih)) && iw && ih &&
            iw <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION && ih <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
            SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            try {
                px.resize(size_t(iw) * ih * 4);
                ok = SUCCEEDED(conv->CopyPixels(nullptr, iw * 4, UINT(px.size()), px.data()));
            } catch (const std::bad_alloc&) {
                ok = false;
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    std::lock_guard<std::mutex> lk(d.mx);
    d.ok = ok;
    if (ok) {
        d.px = std::move(px);
        d.w = iw;
        d.h = ih;
    }
    d.done = true;
}

bool upload(Decoded& d, Tex& t) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = d.w;
    td.Height = d.h;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(gDev->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(gDev->CreateShaderResourceView(tex.Get(), nullptr, &t.srv))) {
        gCtx->UpdateSubresource(tex.Get(), 0, nullptr, d.px.data(), d.w * 4, 0);
        gCtx->GenerateMips(t.srv.Get());
    } else {
        t.srv.Reset();
        td.MipLevels = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = 0;
        D3D11_SUBRESOURCE_DATA sr{};
        sr.pSysMem = d.px.data();
        sr.SysMemPitch = d.w * 4;
        tex.Reset();
        if (FAILED(gDev->CreateTexture2D(&td, &sr, &tex)) || FAILED(gDev->CreateShaderResourceView(tex.Get(), nullptr, &t.srv))) return false;
    }
    t.w = int(d.w);
    t.h = int(d.h);
    if (!gMapSampler) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = 8;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        gDev->CreateSamplerState(&sd, &gMapSampler);
    }
    return true;
}

const Tex* loadTex(const std::string& path, bool* loading = nullptr) {
    if (loading) *loading = false;
    auto it = gTex.find(path);
    if (it == gTex.end()) {
        gTex.clear();
        Tex& t = gTex[path];
        t.pending = std::make_shared<Decoded>();
        try {
            std::thread([path, d = t.pending] { decodeFile(path, *d); }).detach();
        } catch (...) {
            t.pending.reset();
            t.failed = true;
            return nullptr;
        }
        if (loading) *loading = true;
        return nullptr;
    }
    Tex& t = it->second;
    if (t.srv) return &t;
    if (t.failed) return nullptr;
    if (t.pending) {
        std::shared_ptr<Decoded> d = t.pending;
        std::lock_guard<std::mutex> lk(d->mx);
        if (!d->done) {
            if (loading) *loading = true;
            return nullptr;
        }
        t.pending.reset();
        if (!d->ok || !upload(*d, t)) {
            rlog("[-] radar: could not load %s", path.c_str());
            t.failed = true;
            return nullptr;
        }
        d->px.clear();
        d->px.shrink_to_fit();
        rlog("[*] radar: map image %dx%d", t.w, t.h);
        return &t;
    }
    return nullptr;
}

void setMapSampler(const ImDrawList*, const ImDrawCmd*) {
    if (gMapSampler) gCtx->PSSetSamplers(0, 1, gMapSampler.GetAddressOf());
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    const bool imguiMouse = gImguiReady && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
    auto zoom = [](float f) { gZoom = std::clamp(int(float(gZoom) * f), 25, 8000); };
    auto wheelZoom = [&](int delta) { zoom(std::pow(0.8f, float(delta) / float(WHEEL_DELTA))); };
    switch (msg) {
        case WM_INPUT: {
            RAWINPUT ri{};
            UINT sz = sizeof ri;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE && (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)) {
                POINT pt;
                GetCursorPos(&pt);
                const SHORT d = static_cast<SHORT>(ri.data.mouse.usButtonData);
                if (WindowFromPoint(pt) == hwnd) {
                    if (!imguiMouse) wheelZoom(d);
                    else if (GetForegroundWindow() != hwnd) ImGui::GetIO().AddMouseWheelEvent(0.f, float(d) / float(WHEEL_DELTA));
                }
            }
            return DefWindowProcA(hwnd, msg, wp, lp);
        }
        case WM_MOUSEWHEEL:
            if (imguiMouse || gRawWheel) return 0;
            wheelZoom(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_LBUTTONDOWN:
            if (imguiMouse) return 0;
            gDrag = true;
            gDragAt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetCapture(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (gDrag) {
                const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                gPanX -= float(p.x - gDragAt.x) / gK;
                gPanZ += float(p.y - gDragAt.y) / gK;
                gDragAt = p;
            }
            return 0;
        case WM_LBUTTONUP:
            gDrag = false;
            ReleaseCapture();
            return 0;
        case WM_RBUTTONDOWN:
            if (imguiMouse) return 0;
            gPanX = gPanZ = 0;
            return 0;
        case WM_KEYDOWN:
            if (gImguiReady && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) return 0;
            if (wp == VK_SPACE) gPanX = gPanZ = 0;
            else if (wp == VK_TAB) gSideOpen = !gSideOpen;
            else if (wp == VK_ADD || wp == VK_OEM_PLUS) zoom(0.8f);
            else if (wp == VK_SUBTRACT || wp == VK_OEM_MINUS) zoom(1.25f);
            else if (wp == VK_ESCAPE) gWantClose = true;
            return 0;
        case WM_CLOSE: gWantClose = true; return 0;
        case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

bool createWindow(const RECT* target) {
    const HINSTANCE inst = GetModuleHandleA(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = wndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "ickza_radar";
        RegisterClassA(&wc);
        registered = true;
    }
    if (target)
        gWnd = CreateWindowExA(0, "ickza_radar", "Ickza Radar (F8 / Esc hides)", WS_POPUP | WS_VISIBLE, target->left, target->top,
                               target->right - target->left, target->bottom - target->top, nullptr, nullptr, inst, nullptr);
    else
        gWnd = CreateWindowExA(0, "ickza_radar", "Ickza Radar (F8 / Esc hides)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                               1280, 820, nullptr, nullptr, inst, nullptr);
    if (!gWnd) {
        rlog("[-] radar: could not create the window (error %lu)", GetLastError());
        return false;
    }
    const RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_INPUTSINK, gWnd};
    gRawWheel = RegisterRawInputDevices(&rid, 1, sizeof rid) != FALSE;
    if (!gRawWheel) rlog("[!] radar: raw mouse input unavailable (error %lu); wheel zoom needs the radar focused", GetLastError());
    else rlog("[radar] raw mouse input registered");
    return true;
}

bool createDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &gDev, nullptr, &gCtx)))
        return false;
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(gDev.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    RECT rc;
    GetClientRect(gWnd, &rc);
    gW = std::max<LONG>(1, rc.right);
    gH = std::max<LONG>(1, rc.bottom);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = UINT(gW);
    sd.Height = UINT(gH);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(gDev.Get(), gWnd, &sd, nullptr, nullptr, &gSwap))) return false;
    factory->MakeWindowAssociation(gWnd, DXGI_MWA_NO_ALT_ENTER);
    return makeRtv();
}

ImFont* addSystemFont(const char* file, float px) {
    char dir[MAX_PATH] = {};
    GetWindowsDirectoryA(dir, MAX_PATH);
    const std::string path = std::string(dir) + "\\Fonts\\" + file;
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), px);
}

void applyTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0;
    st.ChildRounding = 6;
    st.FrameRounding = 5;
    st.GrabRounding = 5;
    st.PopupRounding = 6;
    st.ScrollbarRounding = 4;
    st.WindowBorderSize = 0;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 0;
    st.WindowPadding = ImVec2(16, 14);
    st.FramePadding = ImVec2(8, 4);
    st.ItemSpacing = ImVec2(8, 6);
    st.ItemInnerSpacing = ImVec2(6, 4);
    st.ScrollbarSize = 8;
    st.GrabMinSize = 10;
    auto v4 = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = v4(th::Text);
    c[ImGuiCol_TextDisabled] = v4(th::Faint);
    c[ImGuiCol_WindowBg] = v4(th::Panel);
    c[ImGuiCol_PopupBg] = v4(IM_COL32(20, 23, 29, 250));
    c[ImGuiCol_Border] = v4(th::Line);
    c[ImGuiCol_FrameBg] = v4(th::Raised);
    c[ImGuiCol_FrameBgHovered] = v4(IM_COL32(37, 42, 52, 255));
    c[ImGuiCol_FrameBgActive] = v4(IM_COL32(44, 50, 62, 255));
    c[ImGuiCol_SliderGrab] = v4(th::Accent);
    c[ImGuiCol_SliderGrabActive] = v4(IM_COL32(120, 245, 225, 255));
    c[ImGuiCol_CheckMark] = v4(th::Accent);
    c[ImGuiCol_Button] = v4(th::Raised);
    c[ImGuiCol_ButtonHovered] = v4(IM_COL32(40, 46, 57, 255));
    c[ImGuiCol_ButtonActive] = v4(IM_COL32(36, 226, 196, 90));
    c[ImGuiCol_Header] = v4(IM_COL32(36, 226, 196, 45));
    c[ImGuiCol_HeaderHovered] = v4(IM_COL32(36, 226, 196, 70));
    c[ImGuiCol_HeaderActive] = v4(IM_COL32(36, 226, 196, 100));
    c[ImGuiCol_Separator] = v4(th::Line);
    c[ImGuiCol_ScrollbarBg] = v4(IM_COL32(0, 0, 0, 0));
    c[ImGuiCol_ScrollbarGrab] = v4(IM_COL32(52, 58, 70, 255));
    c[ImGuiCol_ScrollbarGrabHovered] = v4(IM_COL32(70, 78, 92, 255));
    c[ImGuiCol_ScrollbarGrabActive] = v4(th::Accent);
}

void initImgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    gFont = addSystemFont("segoeui.ttf", 15.f);
    if (!gFont) gFont = io.Fonts->AddFontDefault();
    gBold = addSystemFont("seguisb.ttf", 15.f);
    if (!gBold) gBold = addSystemFont("segoeuib.ttf", 15.f);
    if (!gBold) gBold = gFont;
    ImGui::StyleColorsDark();
    applyTheme();
    ImGui_ImplWin32_Init(gWnd);
    ImGui_ImplDX11_Init(gDev.Get(), gCtx.Get());
    gImguiReady = true;
}

ImVec2 textSize(ImFont* f, float size, const char* s) { return f->CalcTextSizeA(size, FLT_MAX, 0.f, s); }

void haloText(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 c, const char* s) {
    const ImU32 h = IM_COL32(0, 0, 0, int(170.f * float((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.f));
    for (const ImVec2 o : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1), ImVec2(1, 1)}) dl->AddText(f, size, p + o, h, s);
    dl->AddText(f, size, p, c, s);
}

struct Pen {
    ImDrawList* dl;
    ImVec2 c;
    float s;
    ImU32 col, bg;
    ImVec2 P(float x, float y) const { return ImVec2(c.x + x * s, c.y + y * s); }
    void rect(float x0, float y0, float x1, float y1, ImU32 cc, float round = 0) const { dl->AddRectFilled(P(x0, y0), P(x1, y1), cc, round * s); }
    void circle(float x, float y, float r, ImU32 cc) const { dl->AddCircleFilled(P(x, y), r * s, cc, 0); }
    void ring(float x, float y, float r, ImU32 cc, float th) const { dl->AddCircle(P(x, y), r * s, cc, 0, th); }
    void line(float x0, float y0, float x1, float y1, ImU32 cc, float th) const { dl->AddLine(P(x0, y0), P(x1, y1), cc, th); }
    void poly(std::initializer_list<ImVec2> pts, ImU32 cc, bool convex = true) const {
        ImVec2 v[16];
        int n = 0;
        for (const ImVec2& p : pts)
            if (n < 16) v[n++] = P(p.x, p.y);
        if (convex) dl->AddConvexPolyFilled(v, n, cc);
        else dl->AddConcavePolyFilled(v, n, cc);
    }
};

void drawGlyph(ImDrawList* dl, ImVec2 c, float s, int glyph, ImU32 col, ImU32 bg) {
    const Pen g{dl, c, s, col, bg};
    const float stroke = std::max(1.f, s * 0.2f);
    switch (glyph) {
        case loot::Gun:
            g.poly({{-1.0f, -0.22f}, {-0.45f, -0.30f}, {-0.45f, 0.06f}, {-0.95f, 0.30f}}, col);
            g.rect(-0.48f, -0.32f, 0.25f, 0.04f, col);
            g.rect(0.2f, -0.25f, 1.0f, -0.09f, col);
            g.rect(-0.26f, -0.44f, -0.04f, -0.32f, col);
            g.poly({{-0.30f, 0.0f}, {-0.08f, 0.0f}, {-0.16f, 0.50f}, {-0.38f, 0.46f}}, col);
            g.poly({{0.02f, 0.0f}, {0.20f, 0.0f}, {0.32f, 0.44f}, {0.13f, 0.48f}}, col);
            break;
        case loot::Ammo:
            for (int i = -1; i <= 1; ++i) {
                const float x = float(i) * 0.52f, w = 0.18f;
                g.poly({{x - w, -0.08f}, {x - w, -0.32f}, {x - w * 0.55f, -0.64f}, {x, -0.84f}, {x + w * 0.55f, -0.64f}, {x + w, -0.32f}, {x + w, -0.08f}}, col);
                g.rect(x - w, -0.02f, x + w, 0.82f, col);
            }
            break;
        case loot::Attachment:
            g.ring(0, 0, 0.6f, col, stroke);
            g.line(0, -0.98f, 0, -0.3f, col, stroke);
            g.line(0, 0.3f, 0, 0.98f, col, stroke);
            g.line(-0.98f, 0, -0.3f, 0, col, stroke);
            g.line(0.3f, 0, 0.98f, 0, col, stroke);
            g.circle(0, 0, 0.13f, col);
            break;
        case loot::Melee: {
            auto Q = [](float t, float w) { return ImVec2((t + w) * 0.7071f, (-t + w) * 0.7071f); };
            g.poly({Q(-0.05f, -0.15f), Q(0.95f, -0.15f), Q(1.25f, -0.15f), Q(0.85f, 0.22f), Q(-0.05f, 0.3f)}, col);
            g.line(Q(-0.13f, -0.44f).x, Q(-0.13f, -0.44f).y, Q(-0.13f, 0.5f).x, Q(-0.13f, 0.5f).y, col, s * 0.2f);
            g.line(Q(-0.22f, 0.06f).x, Q(-0.22f, 0.06f).y, Q(-1.1f, 0.06f).x, Q(-1.1f, 0.06f).y, col, s * 0.38f);
            g.circle(Q(-0.62f, 0.06f).x, Q(-0.62f, 0.06f).y, 0.07f, bg);
            break;
        }
        case loot::Explosive:
            g.circle(0, 0.24f, 0.62f, col);
            g.rect(-0.2f, -0.62f, 0.2f, -0.32f, col);
            g.poly({{0.12f, -0.62f}, {0.62f, -0.44f}, {0.56f, -0.28f}, {0.12f, -0.42f}}, col);
            g.ring(-0.42f, -0.62f, 0.17f, col, stroke * 0.8f);
            g.line(-0.62f, 0.24f, 0.62f, 0.24f, bg, std::max(1.f, s * 0.1f));
            break;
        case loot::Medical:
            g.rect(-0.25f, -0.8f, 0.25f, 0.8f, col, 0.08f);
            g.rect(-0.8f, -0.25f, 0.8f, 0.25f, col, 0.08f);
            break;
        case loot::Food:
            g.rect(-0.56f, -0.62f, 0.56f, 0.76f, col, 0.14f);
            g.rect(-0.56f, -0.1f, 0.56f, 0.22f, bg);
            g.line(-0.42f, -0.4f, 0.42f, -0.4f, bg, std::max(1.f, s * 0.1f));
            break;
        case loot::Drink:
            g.circle(0, 0.3f, 0.56f, col);
            g.poly({{-0.5f, 0.06f}, {0, -0.9f}, {0.5f, 0.06f}}, col);
            g.circle(-0.2f, 0.36f, 0.14f, bg);
            break;
        case loot::Tool:
            g.line(-0.6f, 0.6f, 0.3f, -0.3f, col, s * 0.34f);
            g.circle(0.42f, -0.42f, 0.44f, col);
            g.circle(0.64f, -0.64f, 0.26f, bg);
            g.circle(-0.62f, 0.62f, 0.2f, col);
            break;
        case loot::Clothing:
            g.poly({{-0.28f, -0.74f}, {-0.94f, -0.42f}, {-0.7f, 0.02f}, {-0.5f, -0.1f}, {-0.5f, 0.8f}, {0.5f, 0.8f}, {0.5f, -0.1f}, {0.7f, 0.02f}, {0.94f, -0.42f}, {0.28f, -0.74f}, {0.0f, -0.5f}},
                   col, false);
            break;
        case loot::Bag:
            g.dl->PathArcTo(g.P(0, -0.42f), 0.3f * s, kPi, 2 * kPi, 12);
            g.dl->PathStroke(col, 0, s * 0.18f);
            g.rect(-0.6f, -0.44f, 0.6f, 0.84f, col, 0.28f);
            g.line(-0.6f, 0.02f, 0.6f, 0.02f, bg, std::max(1.f, s * 0.1f));
            g.dl->AddRect(g.P(-0.34f, 0.24f), g.P(0.34f, 0.62f), bg, 0.1f * s, 0, std::max(1.f, s * 0.1f));
            break;
        case loot::VehiclePart:
            for (int i = 0; i < 8; ++i) {
                const float a = float(i) * kPi / 4.f, dx = std::cos(a), dy = std::sin(a), px = -dy, py = dx;
                g.poly({{dx * 0.5f + px * 0.2f, dy * 0.5f + py * 0.2f}, {dx * 0.95f + px * 0.15f, dy * 0.95f + py * 0.15f},
                        {dx * 0.95f - px * 0.15f, dy * 0.95f - py * 0.15f}, {dx * 0.5f - px * 0.2f, dy * 0.5f - py * 0.2f}},
                       col);
            }
            g.circle(0, 0, 0.66f, col);
            g.circle(0, 0, 0.26f, bg);
            break;
        case loot::Base:
            g.poly({{-0.92f, -0.04f}, {0, -0.9f}, {0.92f, -0.04f}}, col);
            g.rect(-0.62f, -0.1f, 0.62f, 0.82f, col);
            g.rect(-0.18f, 0.32f, 0.18f, 0.82f, bg);
            break;
        case loot::Rare: {
            ImVec2 v[10];
            for (int i = 0; i < 10; ++i) {
                const float a = -kPi / 2 + float(i) * kPi / 5.f, r = (i & 1) ? 0.42f : 1.0f;
                v[i] = g.P(std::cos(a) * r, std::sin(a) * r + 0.06f);
            }
            dl->AddConcavePolyFilled(v, 10, col);
            break;
        }
        case kGlyphPerson:
            g.circle(0, -0.42f, 0.36f, col);
            g.dl->PathArcTo(g.P(0, 0.82f), 0.74f * s, kPi, 2 * kPi, 16);
            g.dl->PathFillConvex(col);
            break;
        case kGlyphPaw:
            g.dl->AddEllipseFilled(g.P(0, 0.36f), ImVec2(0.44f * s, 0.36f * s), col);
            g.circle(-0.6f, -0.1f, 0.18f, col);
            g.circle(-0.22f, -0.5f, 0.19f, col);
            g.circle(0.22f, -0.5f, 0.19f, col);
            g.circle(0.6f, -0.1f, 0.18f, col);
            break;
        case kGlyphCar:
            g.poly({{-0.5f, -0.1f}, {-0.3f, -0.6f}, {0.36f, -0.6f}, {0.6f, -0.1f}}, col);
            g.rect(-0.96f, -0.14f, 0.96f, 0.4f, col, 0.14f);
            g.circle(-0.52f, 0.42f, 0.27f, bg);
            g.circle(0.52f, 0.42f, 0.27f, bg);
            g.circle(-0.52f, 0.42f, 0.18f, col);
            g.circle(0.52f, 0.42f, 0.18f, col);
            break;
        case loot::Other:
        default:
            g.rect(-0.66f, -0.6f, 0.66f, 0.66f, col, 0.12f);
            g.rect(-0.1f, -0.6f, 0.1f, 0.66f, bg);
            g.rect(-0.66f, -0.6f, 0.66f, -0.3f, col, 0.12f);
            g.line(-0.66f, -0.3f, 0.66f, -0.3f, bg, std::max(1.f, s * 0.1f));
            break;
    }
}

void drawBadge(ImDrawList* dl, ImVec2 c, float r, int glyph, ImU32 col, float a = 1.f) {
    dl->AddCircleFilled(c, r + 1.4f, withAlpha(th::Edge, a), 0);
    dl->AddCircleFilled(c, r, withAlpha(th::BadgeFill, a), 0);
    dl->AddCircle(c, r - 0.6f, withAlpha(col, a), 0, std::max(1.2f, r * 0.16f));
    drawGlyph(dl, c, r * 0.56f, glyph, withAlpha(col, a), withAlpha(th::BadgeFill, a));
}

void drawLogo(ImDrawList* dl, ImVec2 c, float r) {
    dl->AddCircleFilled(c, r, IM_COL32(36, 226, 196, 26), 0);
    dl->PathLineTo(c);
    dl->PathArcTo(c, r - 1, -kPi / 2, -kPi / 2 + 1.1f, 16);
    dl->PathFillConvex(IM_COL32(36, 226, 196, 110));
    dl->AddCircle(c, r - 0.5f, th::Accent, 0, 1.8f);
    dl->AddCircle(c, r * 0.5f, IM_COL32(36, 226, 196, 110), 0, 1.f);
    dl->AddLine(c, ImVec2(c.x + std::cos(-kPi / 2 + 1.1f) * (r - 1), c.y + std::sin(-kPi / 2 + 1.1f) * (r - 1)), th::Accent, 1.6f);
    dl->AddCircleFilled(c, 2.f, th::Accent, 0);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.42f, c.y - r * 0.5f), 2.2f, IM_COL32(255, 255, 255, 235), 0);
}

void headingLine(ImDrawList* dl, ImVec2 c, float fx, float fz, float startR, float len, ImU32 col) {
    const float fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 1e-3f) return;
    const ImVec2 d(fx / fl, -fz / fl);
    const ImVec2 a(c.x + d.x * startR, c.y + d.y * startR), b(c.x + d.x * (startR + len), c.y + d.y * (startR + len));
    dl->AddLine(a, b, th::Edge, 4.5f);
    dl->AddLine(a, b, col, 2.f);
    dl->AddCircleFilled(b, 3.4f, th::Edge, 0);
    dl->AddCircleFilled(b, 2.2f, col, 0);
}

float niceStep(float v) {
    const float p = std::pow(10.f, std::floor(std::log10(std::max(v, 1e-3f))));
    for (const float m : {1.f, 2.f, 2.5f, 5.f, 10.f})
        if (m * p >= v) return m * p;
    return 10 * p;
}

struct Counts {
    int cat[loot::Count] = {};
    int lootTotal = 0, lootShown = 0;
    int survivors = 0, infected = 0, animals = 0, vehicles = 0;
    std::map<std::string, int> names;
};

Counts countDots(const Frame& f, const Config& cfg, bool names) {
    Counts n;
    for (const Dot& d : f.dots) switch (d.icon) {
            case Icon::Loot:
                if (d.cat >= 0 && d.cat < loot::Count) ++n.cat[d.cat];
                ++n.lootTotal;
                if (loot::visible(cfg.lootMask, d.cat, d.label)) ++n.lootShown;
                if (names && !d.label.empty()) ++n.names[d.label];
                break;
            case Icon::Survivor: ++n.survivors; break;
            case Icon::Infected: ++n.infected; break;
            case Icon::Animal: ++n.animals; break;
            case Icon::Vehicle: ++n.vehicles; break;
        }
    return n;
}

int filterIndex(const Config& cfg, const std::string& name) {
    for (size_t i = 0; i < cfg.filter.list.size(); ++i) {
        const std::string& p = cfg.filter.list[i].pat;
        if (p.find('*') == std::string::npos && loot::globMatch(p, name)) return int(i);
    }
    return -1;
}

void toggleListed(Config& cfg, const std::string& name) {
    if (name.empty()) return;
    const int i = filterIndex(cfg, name);
    if (i >= 0) {
        cfg.filter.list.erase(cfg.filter.list.begin() + i);
    } else {
        std::string pat = name;
        for (char& c : pat) c = char(std::tolower(static_cast<unsigned char>(c)));
        cfg.filter.list.push_back({pat, true});
        if (cfg.filter.mode == loot::FilterOff) cfg.filter.mode = loot::FilterBlack;
    }
    cfg.changed = true;
}

void hudStat(int glyph, ImU32 col, int value, const char* tip) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(18, 18));
    drawGlyph(ImGui::GetWindowDrawList(), ImVec2(p.x + 9, p.y + 9), 7.f, glyph, col, th::Panel);
    ImGui::SameLine(0, 5);
    char b[16];
    std::snprintf(b, sizeof b, "%d", value);
    ImGui::PushFont(gBold, 15.f);
    ImGui::TextUnformatted(b);
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
}

void drawRadar(const Frame& f, Config& cfg, const Counts& n) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float w = float(gW), h = float(gH);
    const float left = gSideOpen ? kSide : 0.f;
    const float mapW = std::max(1.f, w - left);
    const float R = float(std::max(25, gZoom));
    const float k = std::min(mapW, h) / 2.f / R;
    gK = k;
    const float cX = f.meX + gPanX, cZ = f.meZ + gPanZ;
    const float cx = left + mapW / 2.f, cy = h / 2.f;
    auto sx = [&](float x) { return cx + (x - cX) * k; };
    auto sy = [&](float z) { return cy - (z - cZ) * k; };
    const float scale = std::clamp(cfg.markerScale, 0.5f, 2.5f);
    const float t = float(ImGui::GetTime());
    const float headPx = float(std::clamp(cfg.headingLen, 10, 3000)) * k;

    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), th::Bg);

    bool mapLoading = false, mapMissing = false;
    const float M = f.mapSize;
    const float x0 = cX - (cx - left) / k, x1 = cX + (w - cx) / k, z0 = cZ - h / 2.f / k, z1 = cZ + h / 2.f / k;
    const float mx0 = std::max(0.f, x0), mx1 = std::min(M, x1), mz0 = std::max(0.f, z0), mz1 = std::min(M, z1);
    if (mx1 > mx0 && mz1 > mz0) {
        const std::string png = f.mapBase + ".png";
        const DWORD pa = GetFileAttributesA(png.c_str());
        const bool hasPng = pa != INVALID_FILE_ATTRIBUTES && !(pa & FILE_ATTRIBUTE_DIRECTORY);
        bool loading = false;
        const Tex* tx = loadTex(hasPng ? png : f.mapBase + ".jpg", &loading);
        mapLoading = loading;
        mapMissing = !tx && !loading;
        const ImVec2 pMin(sx(mx0), sy(mz1)), pMax(sx(mx1), sy(mz0));
        if (tx) {
            const ImVec2 uvMin(mx0 / M, (M - mz1) / M), uvMax(mx1 / M, (M - mz0) / M);
            dl->AddCallback(setMapSampler, nullptr);
            dl->AddImage((ImTextureID)(intptr_t)tx->srv.Get(), pMin, pMax, uvMin, uvMax);
            dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
            dl->AddRectFilled(pMin, pMax, IM_COL32(8, 10, 14, 70));
        } else {
            dl->AddRectFilled(pMin, pMax, IM_COL32(22, 26, 32, 255));
        }
    }

    const float step = R > 400 ? 1000.f : 100.f;
    const ImU32 gridCol = IM_COL32(255, 255, 255, step >= 1000 ? 26 : 18);
    const ImU32 gridTxt = IM_COL32(200, 206, 216, 170);
    auto gridLab = [&](float v) {
        static char lab[16];
        std::snprintf(lab, sizeof lab, step >= 1000 ? "%02.0f" : "%03.0f", v / (step >= 1000 ? 1000.f : 100.f));
        return lab;
    };
    for (float gx = std::ceil(x0 / step) * step; gx <= x1; gx += step) {
        const float px = sx(gx);
        if (px < left) continue;
        dl->AddLine(ImVec2(px, 0), ImVec2(px, h), gridCol);
        haloText(dl, gFont, 12.f, ImVec2(px + 4, h - 18), gridTxt, gridLab(gx));
    }
    for (float gz = std::ceil(z0 / step) * step; gz <= z1; gz += step) {
        const float py = sy(gz);
        dl->AddLine(ImVec2(left, py), ImVec2(w, py), gridCol);
        haloText(dl, gFont, 12.f, ImVec2(w - 26, py + 3), gridTxt, gridLab(M - gz));
    }

    const float bx = sx(f.meX), by = sy(f.meZ);

    const int maxTier = R <= 1200 ? 3 : R <= 4000 ? 2 : 1;
    for (const Place& p : f.places) {
        if (p.tier > maxTier) continue;
        const float px = sx(p.x), py = sy(p.z);
        if (px < left + 4 || px > w - 4 || py < 18 || py > h - 18) continue;
        ImFont* fnt = p.tier <= 1 ? gBold : gFont;
        const float size = p.tier == 0 ? 17.f : p.tier == 1 ? 15.f : p.tier == 2 ? 13.5f : 12.5f;
        std::string name = p.name;
        if (p.tier <= 1)
            for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        const ImVec2 sz = textSize(fnt, size, name.c_str());
        haloText(dl, fnt, size, ImVec2(px - sz.x / 2.f, py - sz.y / 2.f), col32(p.color, p.tier == 3 ? 200 : 235), name.c_str());
    }

    const bool lootDots = R > 2200;
    struct Lab {
        ImVec2 at;
        ImU32 col;
        const std::string* text;
    };
    struct Hit {
        ImVec2 p;
        float r;
        const Dot* d;
    };
    std::vector<Lab> labels;
    std::vector<Hit> hits;
    auto order = [](Icon ic) { return ic == Icon::Loot ? 0 : ic == Icon::Animal || ic == Icon::Vehicle ? 1 : ic == Icon::Infected ? 2 : 3; };
    for (int pass = 0; pass < 4; ++pass)
        for (const Dot& d : f.dots) {
            if (order(d.icon) != pass) continue;
            if (d.icon == Icon::Infected && !cfg.infected) continue;
            if (d.icon == Icon::Loot && !loot::visible(cfg.lootMask, d.cat, d.label)) continue;
            const float px = sx(d.x), py = sy(d.z);
            if (px < left - 12 || px >= w + 12 || py < -12 || py >= h + 12) continue;
            const ImVec2 c(px, py);
            float r = 0;
            switch (d.icon) {
                case Icon::Loot: {
                    const ImU32 col = col32(loot::info(d.cat).color);
                    const bool rare = d.cat == loot::Rare;
                    if (rare) {
                        const float pulse = 0.5f + 0.5f * std::sin(t * 3.5f);
                        const float pr = (lootDots ? 5.f : 10.5f) * scale + 2.f + pulse * 4.f;
                        dl->AddCircle(c, pr, withAlpha(col, 0.65f - 0.45f * pulse), 0, 2.f);
                    }
                    if (lootDots) {
                        r = (rare ? 3.6f : 2.6f) * scale;
                        dl->AddCircleFilled(c, r + 1.f, th::Edge, 0);
                        dl->AddCircleFilled(c, r, col, 0);
                    } else {
                        r = (rare ? 10.f : 9.f) * scale;
                        drawBadge(dl, c, r, d.cat, col);
                    }
                    break;
                }
                case Icon::Animal:
                    r = 8.f * scale;
                    drawBadge(dl, c, r, kGlyphPaw, col32(d.color));
                    break;
                case Icon::Vehicle:
                    r = 9.5f * scale;
                    drawBadge(dl, c, r, kGlyphCar, col32(d.color));
                    break;
                case Icon::Infected:
                    r = (lootDots ? 3.f : 4.2f) * scale;
                    dl->AddCircleFilled(c, r + 1.4f, th::Edge, 0);
                    dl->AddCircleFilled(c, r, col32(d.color), 0);
                    break;
                case Icon::Survivor:
                    r = 8.5f * scale;
                    if (cfg.headings) headingLine(dl, c, d.fwdX, d.fwdZ, r + 2.f, std::max(24.f * scale, headPx), col32(d.color));
                    dl->AddCircleFilled(c, r + 3.f, th::Edge, 0);
                    dl->AddCircleFilled(c, r + 1.8f, IM_COL32(255, 255, 255, 240), 0);
                    dl->AddCircleFilled(c, r, col32(d.color), 0);
                    drawGlyph(dl, c, r * 0.6f, kGlyphPerson, IM_COL32(255, 255, 255, 245), col32(d.color));
                    break;
            }
            hits.push_back({c, r, &d});
            if (!cfg.names || d.label.empty()) continue;
            if (d.icon == Icon::Loot && d.cat != loot::Rare) {
                const float dx = d.x - f.meX, dz = d.z - f.meZ;
                if (dx * dx + dz * dz > float(cfg.lootLabelRange) * float(cfg.lootLabelRange)) continue;
            }
            const ImU32 lc = d.icon == Icon::Loot ? col32(loot::info(d.cat).color) : col32(d.color);
            labels.push_back({ImVec2(px + r + 6, py - 8.f), lc, &d.label});
        }

    std::vector<ImVec4> taken;
    for (auto it = labels.rbegin(); it != labels.rend(); ++it) {
        const Lab& L = *it;
        const ImVec2 sz = textSize(gFont, 13.f, L.text->c_str());
        const ImVec4 rc(L.at.x - 4, L.at.y - 2, L.at.x + sz.x + 5, L.at.y + sz.y + 2);
        bool clash = false;
        for (const ImVec4& tk : taken)
            if (rc.x < tk.z && rc.z > tk.x && rc.y < tk.w && rc.w > tk.y) {
                clash = true;
                break;
            }
        if (clash) continue;
        taken.push_back(rc);
        dl->AddRectFilled(ImVec2(rc.x, rc.y), ImVec2(rc.z, rc.w), IM_COL32(12, 14, 18, 215), 4.f);
        dl->AddRectFilled(ImVec2(rc.x, rc.y), ImVec2(rc.x + 3.f, rc.w), L.col, 4.f, ImDrawFlags_RoundCornersLeft);
        dl->AddText(gFont, 13.f, L.at, th::Text, L.text->c_str());
    }

    float fx = f.fwdX, fz = f.fwdZ;
    const float fl = std::sqrt(fx * fx + fz * fz);
    if (fl > 1e-3f) {
        fx /= fl;
        fz /= fl;
        const ImVec2 fwd(fx, -fz), rt(-fz, -fx);
        if (cfg.headings) headingLine(dl, ImVec2(bx, by), fx, fz, 15.f, std::max(36.f, headPx), IM_COL32(255, 255, 255, 255));
        const float len = 14.f, wide = 8.f;
        const ImVec2 tip(bx + fwd.x * len, by + fwd.y * len);
        const ImVec2 back(bx - fwd.x * 3.f, by - fwd.y * 3.f);
        const ImVec2 lw(bx - fwd.x * 7 + rt.x * wide, by - fwd.y * 7 + rt.y * wide);
        const ImVec2 rw(bx - fwd.x * 7 - rt.x * wide, by - fwd.y * 7 - rt.y * wide);
        const ImVec2 arrow[4] = {tip, lw, back, rw};
        dl->AddConcavePolyFilled(arrow, 4, IM_COL32(255, 255, 255, 255));
        dl->AddPolyline(arrow, 4, IM_COL32(8, 30, 28, 255), ImDrawFlags_Closed, 1.8f);
    } else {
        dl->AddCircleFilled(ImVec2(bx, by), 6.f, IM_COL32(255, 255, 255, 255), 0);
        dl->AddCircle(ImVec2(bx, by), 6.f, IM_COL32(8, 30, 28, 255), 0, 1.8f);
    }
    if (cfg.names && !f.meName.empty()) {
        const ImVec2 at(bx + 20.f, by - 8.f);
        const ImVec2 sz = textSize(gFont, 13.f, f.meName.c_str());
        const ImVec4 rc(at.x - 4, at.y - 2, at.x + sz.x + 5, at.y + sz.y + 2);
        dl->AddRectFilled(ImVec2(rc.x, rc.y), ImVec2(rc.z, rc.w), IM_COL32(12, 14, 18, 215), 4.f);
        dl->AddRectFilled(ImVec2(rc.x, rc.y), ImVec2(rc.x + 3.f, rc.w), IM_COL32(255, 255, 255, 255), 4.f, ImDrawFlags_RoundCornersLeft);
        dl->AddText(gFont, 13.f, at, th::Text, f.meName.c_str());
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && !gDrag && io.MousePos.x > left) {
        const Hit* best = nullptr;
        float bd = FLT_MAX;
        for (const Hit& hh : hits) {
            const float dx = hh.p.x - io.MousePos.x, dy = hh.p.y - io.MousePos.y, dd = dx * dx + dy * dy;
            const float lim = hh.r + 6.f;
            if (dd < lim * lim && dd < bd) {
                bd = dd;
                best = &hh;
            }
        }
        if (best) {
            const Dot& d = *best->d;
            const float dist = std::sqrt((d.x - f.meX) * (d.x - f.meX) + (d.z - f.meZ) * (d.z - f.meZ));
            ImGui::BeginTooltip();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(30, 30));
            const bool isLoot = d.icon == Icon::Loot;
            const int glyph = isLoot ? d.cat : d.icon == Icon::Animal ? kGlyphPaw : d.icon == Icon::Vehicle ? kGlyphCar : kGlyphPerson;
            const ImU32 col = isLoot ? col32(loot::info(d.cat).color) : col32(d.color);
            drawBadge(ImGui::GetWindowDrawList(), ImVec2(p.x + 15, p.y + 15), 13.f, glyph, col);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::PushFont(gBold, 15.f);
            const char* kind = isLoot ? loot::info(d.cat).name
                                      : d.icon == Icon::Survivor ? "Survivor"
                                      : d.icon == Icon::Infected ? "Infected"
                                      : d.icon == Icon::Animal   ? "Animal"
                                                                 : "Vehicle";
            ImGui::TextUnformatted(d.label.empty() ? kind : d.label.c_str());
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
            ImGui::Text("%s", kind);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("%.0f m", dist);
            if (isLoot && !d.label.empty()) {
                const bool inList = filterIndex(cfg, d.label) >= 0;
                const bool white = cfg.filter.mode == loot::FilterWhite;
                ImGui::TextDisabled("F  %s", inList ? (white ? "remove from whitelist" : "unhide (remove from blacklist)")
                                                    : (white ? "add to whitelist" : "hide this item (blacklist)"));
            }
            ImGui::EndGroup();
            ImGui::EndTooltip();
            if (isLoot && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) toggleListed(cfg, d.label);
        }
    }

    {
        const float nx = cx, ny = 16.f;
        dl->AddTriangleFilled(ImVec2(nx, ny - 8), ImVec2(nx + 6, ny + 3), ImVec2(nx - 6, ny + 3), th::Accent);
        const ImVec2 nsz = textSize(gBold, 13.f, "N");
        haloText(dl, gBold, 13.f, ImVec2(nx - nsz.x / 2.f, ny + 5), th::Text, "N");
    }

    {
        std::string msg;
        float frac = -1;
        ImU32 col = th::Text;
        if (cfg.dl.busy) {
            frac = cfg.dl.total ? float(cfg.dl.done) / float(cfg.dl.total) : 0.f;
            char b[160];
            if (cfg.dl.total)
                std::snprintf(b, sizeof b, "Downloading %s map   %d%%   %.1f MB/s", cfg.dl.label.c_str(), int(frac * 100.f + 0.5f), cfg.dl.mbps);
            else
                std::snprintf(b, sizeof b, "%s %s map...", cfg.dl.phase.c_str(), cfg.dl.label.c_str());
            msg = b;
        } else if (mapLoading) {
            msg = "Loading map...";
        } else if (mapMissing) {
            msg = cfg.dl.error.empty() ? "No map image - pick a map under MAP to download it" : "Map download failed: " + cfg.dl.error;
            col = cfg.dl.error.empty() ? th::Muted : IM_COL32(255, 140, 120, 255);
        }
        if (!msg.empty()) {
            const ImVec2 ts = textSize(gFont, 13.f, msg.c_str());
            const float bw = std::max(ts.x + 28.f, 260.f), pbx = cx - bw / 2.f, pby = 40.f;
            const float bh = frac >= 0 ? 38.f : 28.f;
            dl->AddRectFilled(ImVec2(pbx, pby), ImVec2(pbx + bw, pby + bh), IM_COL32(12, 14, 18, 225), 8.f);
            dl->AddText(gFont, 13.f, ImVec2(cx - ts.x / 2.f, pby + 6), col, msg.c_str());
            if (frac >= 0) {
                const float px0 = pbx + 14, px1 = pbx + bw - 14, py = pby + 27;
                dl->AddRectFilled(ImVec2(px0, py), ImVec2(px1, py + 4), th::Line, 2.f);
                dl->AddRectFilled(ImVec2(px0, py), ImVec2(px0 + (px1 - px0) * std::clamp(frac, 0.f, 1.f), py + 4), th::Accent, 2.f);
            }
        }
    }

    {
        const float target = 140.f / k;
        const float L = niceStep(target * 0.7f);
        const float px = L * k;
        const float x = left + 22, y = h - 34;
        dl->AddRectFilled(ImVec2(x - 10, y - 22), ImVec2(x + px + 12, y + 10), IM_COL32(12, 14, 18, 200), 6.f);
        dl->AddLine(ImVec2(x, y), ImVec2(x + px, y), th::Text, 2.f);
        dl->AddLine(ImVec2(x, y - 5), ImVec2(x, y + 2), th::Text, 2.f);
        dl->AddLine(ImVec2(x + px, y - 5), ImVec2(x + px, y + 2), th::Text, 2.f);
        dl->AddLine(ImVec2(x + px / 2, y - 3), ImVec2(x + px / 2, y), th::Muted, 1.5f);
        char lab[24];
        if (L >= 1000) std::snprintf(lab, sizeof lab, "%.0f km", L / 1000.f);
        else std::snprintf(lab, sizeof lab, "%.0f m", L);
        dl->AddText(gBold, 12.f, ImVec2(x, y - 19), th::Text, lab);
    }

    if (!gSideOpen) {
        const char* help = "wheel zoom    drag pan    right-click / space recentre    Tab panel    F8 / Esc hide";
        const ImVec2 hs = textSize(gFont, 12.f, help);
        haloText(dl, gFont, 12.f, ImVec2(w - hs.x - 40, h - 40), IM_COL32(170, 176, 188, 200), help);
    }

    ImGui::SetNextWindowPos(ImVec2(w - 14, 12), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 4));
    if (ImGui::Begin("##hud", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
        hudStat(kGlyphPerson, col32(th::Survivor), n.survivors, "survivors");
        ImGui::SameLine();
        hudStat(kGlyphPerson, col32(th::Infected), n.infected, "infected");
        ImGui::SameLine();
        hudStat(kGlyphCar, col32(th::Vehicle), n.vehicles, "vehicles");
        ImGui::SameLine();
        hudStat(kGlyphPaw, col32(th::Animal), n.animals, "animals");
        ImGui::SameLine();
        hudStat(loot::Other, th::Accent, n.lootShown, "loot shown (of all loot in range)");
        ImGui::SameLine(0, 2);
        ImGui::TextDisabled("/ %d", n.lootTotal);
        ImGui::TextDisabled("GRID");
        ImGui::SameLine(0, 6);
        ImGui::Text("%03.0f %03.0f", f.meX / 100.f, (M - f.meZ) / 100.f);
        ImGui::SameLine();
        ImGui::TextDisabled("POS");
        ImGui::SameLine(0, 6);
        ImGui::Text("%05.0f / %05.0f", f.meX, f.meZ);
        ImGui::SameLine();
        ImGui::TextDisabled("ZOOM");
        ImGui::SameLine(0, 6);
        ImGui::Text("%d m", gZoom);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

bool toggleSwitch(const char* id, bool* v) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float sw = 32.f, sh = 18.f;
    ImGui::InvisibleButton(id, ImVec2(sw, sh));
    const bool clicked = ImGui::IsItemClicked(0);
    if (clicked) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 track = *v ? th::Accent : (ImGui::IsItemHovered() ? IM_COL32(64, 71, 85, 255) : IM_COL32(52, 58, 70, 255));
    dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sh), track, sh / 2);
    dl->AddCircleFilled(ImVec2(p.x + (*v ? sw - sh / 2 : sh / 2), p.y + sh / 2), sh / 2 - 3.f, *v ? IM_COL32(10, 30, 27, 255) : th::Text, 0);
    return clicked;
}

bool switchRow(const char* label, bool* v) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(right - 32.f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.f);
    ImGui::PushID(label);
    const bool r = toggleSwitch("##sw", v);
    ImGui::PopID();
    return r;
}

void section(const char* title) {
    ImGui::Dummy(ImVec2(0, 6));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(gBold, 12.f, p, th::Muted, title);
    const float tw = textSize(gBold, 12.f, title).x;
    dl->AddLine(ImVec2(p.x + tw + 10, p.y + 7), ImVec2(p.x + avail, p.y + 7), th::Line);
    ImGui::Dummy(ImVec2(avail, 16));
}

int catRow(const char* id, int glyph, ImU32 col, const char* name, int count, bool on, bool canSolo, bool canToggle = true) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rw = ImGui::GetContentRegionAvail().x, rh = 28.f;
    ImGui::InvisibleButton(id, ImVec2(rw, rh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + rw, p.y + rh);
    if (on) dl->AddRectFilled(p, q, withAlpha(col, 0.11f), 6.f);
    if (hov) dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 12), 6.f);
    if (on) dl->AddRectFilled(ImVec2(p.x, p.y + 6), ImVec2(p.x + 3, q.y - 6), col, 2.f);
    drawBadge(dl, ImVec2(p.x + 20, p.y + rh / 2), 10.f, glyph, col, on ? 1.f : 0.32f);
    dl->AddText(on ? gBold : gFont, 15.f, ImVec2(p.x + 38, p.y + rh / 2 - 8.5f), on ? th::Text : th::Faint, name);

    const ImVec2 pillA(q.x - 50, p.y + 5), pillB(q.x - 6, q.y - 5);
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool overPill = canSolo && hov && mp.x >= pillA.x && mp.x <= pillB.x;
    if (canSolo && hov) {
        dl->AddRectFilled(pillA, pillB, overPill ? th::Accent : IM_COL32(52, 58, 70, 255), 9.f);
        const ImVec2 ts = textSize(gBold, 11.f, "ONLY");
        dl->AddText(gBold, 11.f, ImVec2((pillA.x + pillB.x - ts.x) / 2, (pillA.y + pillB.y - ts.y) / 2), overPill ? IM_COL32(10, 30, 27, 255) : th::Text, "ONLY");
    } else {
        char b[16];
        std::snprintf(b, sizeof b, "%d", count);
        const ImVec2 ts = textSize(gFont, 14.f, b);
        dl->AddText(gFont, 14.f, ImVec2(q.x - 10 - ts.x, p.y + rh / 2 - ts.y / 2), count ? (on ? th::Text : th::Faint) : th::Faint, b);
    }
    if (canSolo && ImGui::IsItemClicked(1)) return 2;
    if (ImGui::IsItemClicked(0)) return overPill ? 2 : canToggle ? 1 : 0;
    return 0;
}

bool chip(const char* label, bool lit, float width) {
    ImGui::PushStyleColor(ImGuiCol_Button, lit ? ImGui::ColorConvertU32ToFloat4(IM_COL32(36, 226, 196, 60)) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(lit ? th::Accent : th::Text));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.f);
    const bool r = ImGui::Button(label, ImVec2(width, 24));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return r;
}

unsigned maskOf(std::initializer_list<int> cats) {
    unsigned m = 0;
    for (const int c : cats) m |= 1u << c;
    return m;
}

void onlineCombo(const std::vector<Online>& online) {
    char preview[48];
    std::snprintf(preview, sizeof preview, online.empty() ? "Online: not read yet" : "Online: %zu players", online.size());
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SetNextItemWidth(-1);
    static char search[48] = "";
    if (!ImGui::BeginCombo("##online", preview, ImGuiComboFlags_HeightLarge)) return;
    if (ImGui::IsWindowAppearing()) {
        search[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##onlinesearch", "search players...", search, sizeof search);
    std::string needle = search;
    for (char& c : needle) c = char(std::tolower(static_cast<unsigned char>(c)));

    std::vector<const Online*> rows;
    for (const Online& o : online) {
        if (!needle.empty()) {
            std::string hay = o.name;
            for (char& c : hay) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (hay.find(needle) == std::string::npos) continue;
        }
        rows.push_back(&o);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Online* a, const Online* b) {
        const int ra = a->me ? 0 : a->dist >= 0 ? 1 : 2, rb = b->me ? 0 : b->dist >= 0 ? 1 : 2;
        if (ra != rb) return ra < rb;
        return ra == 1 && a->dist < b->dist;
    });

    int lastGroup = -1;
    for (const Online* o : rows) {
        const int group = o->me || o->dist >= 0 ? 0 : 1;
        if (group != lastGroup) {
            ImGui::SeparatorText(group == 0 ? "Near you" : "Elsewhere");
            lastGroup = group;
        }
        const ImU32 col = o->me ? th::Accent : o->dist >= 0 ? col32(th::Survivor) : th::Text;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
        ImGui::TextUnformatted(o->name.c_str());
        ImGui::PopStyleColor();
        const char* tag = nullptr;
        char b[16];
        if (o->me) tag = "you";
        else if (o->dist >= 0) std::snprintf(b, sizeof b, "%.0f m", o->dist), tag = b;
        if (tag) {
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(tag).x - 4.f);
            ImGui::TextDisabled("%s", tag);
        }
    }
    if (rows.empty()) ImGui::TextDisabled(online.empty() ? "join a server to see players" : "no player matches");
    ImGui::EndCombo();
}

void drawSidebar(Config& cfg, const Counts& n, bool live, const std::vector<Online>& online) {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSide, float(gH)), ImGuiCond_Always);
    if (!ImGui::Begin("##side", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::End();
        return;
    }
    bool ch = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        drawLogo(dl, ImVec2(p.x + 15, p.y + 16), 15.f);
        dl->AddText(gBold, 24.f, ImVec2(p.x + 40, p.y + 1), th::Text, "ICKZA");
        const float bw = textSize(gBold, 24.f, "ICKZA").x;
        dl->AddText(gBold, 12.f, ImVec2(p.x + 42 + bw + 6, p.y + 12), th::Accent, "RADAR");
        const char* st = live ? "LIVE" : "WAITING";
        const ImU32 sc = live ? IM_COL32(80, 230, 130, 255) : col32(th::Infected);
        const float sw = textSize(gBold, 11.f, st).x;
        const ImVec2 a(p.x + avail - sw - 24, p.y + 8), b(p.x + avail, p.y + 26);
        dl->AddRectFilled(a, b, withAlpha(sc, 0.14f), 9.f);
        const float pulse = live ? 0.55f + 0.45f * std::sin(float(ImGui::GetTime()) * 4.f) : 1.f;
        dl->AddCircleFilled(ImVec2(a.x + 9, (a.y + b.y) / 2), 3.f, withAlpha(sc, pulse), 0);
        dl->AddText(gBold, 11.f, ImVec2(a.x + 16, a.y + 3), sc, st);
        ImGui::Dummy(ImVec2(avail, 34));
    }

    section("ESP OVERLAY");
    {
        Config::Esp& e = cfg.esp;
        ch |= switchRow("Skeleton", &e.skeleton);
        ch |= switchRow("Box", &e.box);
        ch |= switchRow("Names", &e.names);
        ch |= switchRow("Distance", &e.distance);
        ch |= switchRow("Held item", &e.held);
        ch |= switchRow("Zombies", &cfg.infected);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("infected on the overlay, minimap and radar; off also stops reading them");
        ch |= switchRow("Valuable loot only", &e.valuables);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("overlay loot: guns, ammo, attachments, explosives, bags, rare");
        ch |= switchRow("FPS counter", &e.fps);
        ch |= switchRow("Uncapped FPS", &e.uncapped);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("no vsync: newest frame each refresh");
        ch |= switchRow("Corner minimap", &e.minimap);
        ch |= switchRow("Crosshair", &e.crosshair);
        if (e.crosshair) {
            ImGui::PushItemWidth(-1);
            ch |= ImGui::SliderInt("##xhair", &e.crosshairSize, 4, 40, "size %d px");
            ImGui::PopItemWidth();
        }

        int v = std::clamp(e.range, 25, kMaxRange);
        ImGui::TextDisabled("View distance");
        ImGui::PushItemWidth(-1);
        if (ImGui::SliderInt("##range", &v, 25, kMaxRange, "%d m", ImGuiSliderFlags_Logarithmic)) {
            e.range = v;
            ch = true;
        }
        ImGui::PopItemWidth();
    }

    section("LOOT");
    {
        const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Ground loot");
        ImGui::SameLine();
        ImGui::TextDisabled("%d / %d", n.lootShown, n.lootTotal);
        ImGui::SameLine(right - 32.f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.f);
        ch |= toggleSwitch("##loot", &cfg.loot);
    }
    ImGui::BeginDisabled(!cfg.loot);
    {
        const unsigned valuables = maskOf({loot::Gun, loot::Ammo, loot::Attachment, loot::Explosive, loot::Bag, loot::Rare});
        const unsigned survival = maskOf({loot::Food, loot::Drink, loot::Medical, loot::Tool});
        const float cw = (ImGui::GetContentRegionAvail().x - 3 * 6.f) / 4.f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
        if (chip("All", cfg.lootMask == loot::kAll, cw)) cfg.lootMask = loot::kAll, ch = true;
        ImGui::SameLine();
        if (chip("None", cfg.lootMask == 0, cw)) cfg.lootMask = 0, ch = true;
        ImGui::SameLine();
        if (chip("Gear", cfg.lootMask == valuables, cw)) cfg.lootMask = valuables, ch = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("guns, ammo, attachments, explosives, bags, rare");
        ImGui::SameLine();
        if (chip("Survive", cfg.lootMask == survival, cw)) cfg.lootMask = survival, ch = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("food, drink, medical, tools");
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0, 2));

        static const int kOrder[] = {loot::Rare,    loot::Gun,      loot::Ammo, loot::Attachment, loot::Explosive,   loot::Melee, loot::Medical, loot::Food,
                                     loot::Drink,   loot::Tool,     loot::Bag,  loot::Clothing,   loot::VehiclePart, loot::Base,  loot::Other};
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
        for (const int c : kOrder) {
            const loot::Info& li = loot::info(c);
            const bool on = loot::shown(cfg.lootMask, c);
            const int r = catRow(li.id, c, col32(li.color), li.name, n.cat[c], on, true);
            if (r == 1) cfg.lootMask ^= 1u << c, ch = true;
            else if (r == 2) {
                const unsigned only = 1u << c;
                cfg.lootMask = cfg.lootMask == only ? loot::kAll : only;
                ch = true;
            }
        }
        ImGui::PopStyleVar();
        ImGui::TextDisabled("click toggles, right-click or ONLY shows just that");
    }
    ImGui::EndDisabled();

    section("ITEM FILTER");
    ImGui::BeginDisabled(!cfg.loot);
    {
        loot::Filter& fl = cfg.filter;
        const float cw = (ImGui::GetContentRegionAvail().x - 2 * 6.f) / 3.f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
        if (chip("Off", fl.mode == loot::FilterOff, cw)) fl.mode = loot::FilterOff, ch = true;
        ImGui::SameLine();
        if (chip("Blacklist", fl.mode == loot::FilterBlack, cw)) fl.mode = loot::FilterBlack, ch = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("hide the listed items; the categories above still apply");
        ImGui::SameLine();
        if (chip("Whitelist", fl.mode == loot::FilterWhite, cw)) fl.mode = loot::FilterWhite, ch = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("show ONLY the listed items, whatever the categories say");
        ImGui::PopStyleVar();

        static char add[96] = {};
        auto addPat = [&](std::string p) {
            for (char& c : p) c = char(std::tolower(static_cast<unsigned char>(c)));
            while (!p.empty() && p.back() == ' ') p.pop_back();
            while (!p.empty() && p.front() == ' ') p.erase(0, 1);
            if (p.empty() || p.find('|') != std::string::npos) return;
            for (const loot::FilterEntry& e : fl.list)
                if (e.pat == p) return;
            fl.list.push_back({p, true});
            if (fl.mode == loot::FilterOff) fl.mode = loot::FilterBlack;
            ch = true;
        };
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 34.f);
        const bool enter = ImGui::InputTextWithHint("##filteradd", "classname, * = wildcard", add, sizeof add, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        ImGui::SameLine(0, 6);
        if ((ImGui::Button("+", ImVec2(28, 0)) || enter) && add[0]) {
            addPat(add);
            add[0] = 0;
        }
        if (add[0]) {
            std::string q = add;
            for (char& c : q) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (q.find('*') == std::string::npos) q = "*" + q + "*";
            int shown = 0;
            for (const auto& [name, cnt] : n.names) {
                if (!loot::globMatch(q, name)) continue;
                if (++shown > 8) break;
                char row[160];
                std::snprintf(row, sizeof row, "%s##sug%d", name.c_str(), shown);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable(row, false)) {
                    addPat(name);
                    add[0] = 0;
                }
                char b[16];
                std::snprintf(b, sizeof b, "%d", cnt);
                const ImVec2 ts = textSize(gFont, 14.f, b);
                ImGui::GetWindowDrawList()->AddText(gFont, 14.f, ImVec2(p.x + ImGui::GetContentRegionAvail().x - ts.x - 4, p.y), th::Faint, b);
            }
            if (!shown) ImGui::TextDisabled("nothing in range matches; Enter adds it anyway");
        }

        int drop = -1;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 3));
        for (size_t i = 0; i < fl.list.size(); ++i) {
            loot::FilterEntry& e = fl.list[i];
            ImGui::PushID(int(i));
            int hits = 0;
            for (const auto& [name, cnt] : n.names)
                if (loot::globMatch(e.pat, name)) hits += cnt;
            const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            ch |= toggleSwitch("##on", &e.on);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            char b[16];
            std::snprintf(b, sizeof b, "%d", hits);
            const float countX = right - 30.f - ImGui::CalcTextSize(b).x;
            std::string shown = e.pat;
            const float room = countX - 8.f - ImGui::GetCursorPosX();
            if (ImGui::CalcTextSize(shown.c_str()).x > room) {
                while (shown.size() > 1 && ImGui::CalcTextSize((shown + "...").c_str()).x > room) shown.pop_back();
                shown += "...";
            }
            if (e.on) ImGui::TextUnformatted(shown.c_str());
            else ImGui::TextDisabled("%s", shown.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%d in range", e.pat.c_str(), hits);
            ImGui::SameLine(countX);
            ImGui::TextDisabled("%s", b);
            ImGui::SameLine(right - 20.f);
            if (ImGui::SmallButton("x")) drop = int(i);
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        if (drop >= 0) fl.list.erase(fl.list.begin() + drop), ch = true;

        if (fl.list.empty()) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("Type a classname above, or hover an item on the map and press F.");
            ImGui::PopTextWrapPos();
        } else if (chip("Clear list", false, ImGui::GetContentRegionAvail().x)) {
            fl.list.clear();
            ch = true;
        }
    }
    ImGui::EndDisabled();

    section("PLAYERS & WORLD");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
    catRow("##surv", kGlyphPerson, col32(th::Survivor), "Survivors", n.survivors, true, false, false);
    if (catRow("##inf", kGlyphPerson, col32(th::Infected), "Infected", n.infected, cfg.infected, false)) cfg.infected = !cfg.infected, ch = true;
    if (catRow("##veh", kGlyphCar, col32(th::Vehicle), "Vehicles", n.vehicles, cfg.vehicles, false)) cfg.vehicles = !cfg.vehicles, ch = true;
    if (catRow("##ani", kGlyphPaw, col32(th::Animal), "Animals", n.animals, cfg.animals, false)) cfg.animals = !cfg.animals, ch = true;
    ImGui::PopStyleVar();
    onlineCombo(online);

    section("DISPLAY");
    ImGui::PushItemWidth(-1);
    ImGui::TextDisabled("Zoom");
    if (ImGui::SliderInt("##zoom", &gZoom, 25, 8000, "%d m", ImGuiSliderFlags_Logarithmic)) ch = true;
    ImGui::TextDisabled("Marker size");
    ch |= ImGui::SliderFloat("##marker", &cfg.markerScale, 0.5f, 2.5f, "%.1fx");
    ch |= switchRow("Labels", &cfg.names);
    ImGui::BeginDisabled(!cfg.names);
    ImGui::TextDisabled("Label loot within");
    ch |= ImGui::SliderInt("##lootlab", &cfg.lootLabelRange, 25, 2000, "%d m", ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    ch |= switchRow("Heading lines", &cfg.headings);
    ImGui::BeginDisabled(!cfg.headings);
    ImGui::TextDisabled("Heading line length");
    ch |= ImGui::SliderInt("##headlen", &cfg.headingLen, 10, 3000, "%d m", ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();

    section("MAP");
    bool curInstalled = false;
    std::string curLabel = cfg.map;
    for (const Config::MapItem& m : cfg.mapList)
        if (m.id == cfg.map) {
            curLabel = m.label;
            curInstalled = m.installed;
        }
    static char mapSearch[48] = "";
    if (ImGui::BeginCombo("##map", curLabel.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            mapSearch[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##mapsearch", "search maps...", mapSearch, sizeof mapSearch);
        std::string needle = mapSearch;
        for (char& c : needle) c = char(std::tolower(static_cast<unsigned char>(c)));
        for (int pass = 0; pass < 2; ++pass) {
            const bool official = pass == 0;
            bool any = false;
            for (const Config::MapItem& m : cfg.mapList) {
                if (m.official != official) continue;
                if (!needle.empty()) {
                    std::string hay = m.label + " " + m.id;
                    for (char& c : hay) c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (hay.find(needle) == std::string::npos) continue;
                }
                if (!any) ImGui::SeparatorText(official ? "Official" : "Community");
                any = true;
                const bool installed = m.installed;
                const bool sel = m.id == cfg.map;
                const std::string item = m.label + "##" + m.id;
                if (ImGui::Selectable(item.c_str(), sel) && !sel) {
                    cfg.map = m.id;
                    ch = true;
                }
                if (!installed) {
                    const ImVec2 r1 = ImGui::GetItemRectMax(), r0 = ImGui::GetItemRectMin();
                    const float ax = r1.x - 10.f, ay = (r0.y + r1.y) / 2.f;
                    ImDrawList* wl = ImGui::GetWindowDrawList();
                    wl->AddLine(ImVec2(ax, ay - 5), ImVec2(ax, ay + 3), th::Muted, 1.5f);
                    wl->AddTriangleFilled(ImVec2(ax - 4, ay + 1), ImVec2(ax + 4, ay + 1), ImVec2(ax, ay + 6), th::Muted);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("type to search; maps with an arrow are downloaded when picked (a few seconds, from dayz.xam.nu)");
    ImGui::PopItemWidth();

    ch |= switchRow("Follow the game's map", &cfg.mapAuto);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("switch the radar to the map the game loads (downloading it if needed).\n"
                          "A map you pick by hand stays until you join a different map.");
    if (cfg.mapDetected.empty()) ImGui::TextDisabled("Game map: not read yet");
    else ImGui::TextDisabled("Game map: %s", cfg.mapDetected.c_str());

    {
        const float gap = ImGui::GetStyle().ItemSpacing.x, half = (ImGui::GetContentRegionAvail().x - gap) / 2.f;
        if (chip("Satellite", !cfg.mapTopo, half) && cfg.mapTopo) {
            cfg.mapTopo = false;
            ch = true;
        }
        ImGui::SameLine();
        if (chip("Topographic", cfg.mapTopo, half) && !cfg.mapTopo) {
            cfg.mapTopo = true;
            ch = true;
        }
    }
    ch |= switchRow("High detail downloads", &cfg.mapHigh);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("16384 px instead of 8192: sharper when zoomed right in, 4x the download and ~1.4 GB of GPU memory.\n"
                          "Applies to the next download; use Re-download for a map you already have.");

    if (cfg.dl.busy) {
        const float frac = cfg.dl.total ? float(cfg.dl.done) / float(cfg.dl.total) : 0.f;
        char ov[96];
        if (cfg.dl.total) std::snprintf(ov, sizeof ov, "%s  %d / %d", cfg.dl.label.c_str(), cfg.dl.done, cfg.dl.total);
        else std::snprintf(ov, sizeof ov, "%s %s...", cfg.dl.phase.c_str(), cfg.dl.label.c_str());
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::ColorConvertU32ToFloat4(IM_COL32(36, 226, 196, 150)));
        ImGui::ProgressBar(frac, ImVec2(-1, 20), ov);
        ImGui::PopStyleColor();
        if (chip("Cancel download", false, ImGui::GetContentRegionAvail().x)) cfg.cancelReq = true;
    } else {
        if (!cfg.dl.error.empty()) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 140, 120, 255)), "%s", cfg.dl.error.c_str());
            ImGui::PopTextWrapPos();
        }
        if (chip(curInstalled ? "Re-download map" : !cfg.dl.error.empty() ? "Retry download" : "Download map", false, ImGui::GetContentRegionAvail().x))
            cfg.downloadReq = cfg.map;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(curInstalled ? "fetch this map again (e.g. after turning on High detail)" : "fetch this map now");
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (chip("Setup window", false, ImGui::GetContentRegionAvail().x)) cfg.showSetup = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("bring the Ickza setup window (PID, server, DMA speed, Start/Stop) to the front");
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("Wheel zoom, drag pan, right-click or Space recentres. Tab hides this panel, F8 or Esc hides the radar.");
    ImGui::PopTextWrapPos();

    if (ch) cfg.changed = true;
    ImGui::End();
}

void drawWaiting() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float w = float(gW), h = float(gH);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), th::Bg);
    const ImVec2 c(w / 2.f, h / 2.f - 30.f);
    const float t = float(ImGui::GetTime());
    for (int i = 0; i < 3; ++i) {
        const float ph = std::fmod(t * 0.6f + float(i) / 3.f, 1.f);
        dl->AddCircle(c, 30.f + ph * 120.f, IM_COL32(36, 226, 196, int(90 * (1 - ph))), 0, 1.5f);
    }
    drawLogo(dl, c, 30.f);
    const ImVec2 bs = textSize(gBold, 34.f, "ICKZA");
    dl->AddText(gBold, 34.f, ImVec2(c.x - bs.x / 2, c.y + 46), th::Text, "ICKZA");
    const char* sub = "waiting for DayZ  -  camera not read yet";
    const ImVec2 ss = textSize(gFont, 14.f, sub);
    dl->AddText(gFont, 14.f, ImVec2(c.x - ss.x / 2, c.y + 90), th::Muted, sub);
}

}

bool visible() { return gWnd != nullptr; }

void reloadMaps() { gTex.clear(); }

void destroy() {
    if (gRawWheel) {
        const RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&rid, 1, sizeof rid);
        gRawWheel = false;
    }
    if (gImguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        gImguiReady = false;
    }
    gFont = gBold = nullptr;
    gTex.clear();
    gMapSampler.Reset();
    gRtv.Reset();
    gSwap.Reset();
    gCtx.Reset();
    gDev.Reset();
    if (gWnd) DestroyWindow(gWnd);
    gWnd = nullptr;
    gDrag = false;
    gWantClose = false;
}

bool sync(bool& want, int& zoomMetres, Config& cfg, const Frame& frame, const RECT* target) {
    cfg.changed = false;
    if (!want) {
        if (gWnd) destroy();
        return true;
    }
    if (!gWnd) {
        if (!createWindow(target)) return false;
        if (!createDevice()) {
            rlog("[-] radar: no GPU device");
            destroy();
            return false;
        }
        initImgui();
        gPanX = gPanZ = 0;
        rlog("[+] radar screen (ImGui) %dx%d", gW, gH);
    }

    static int handedBack = -1;
    if (zoomMetres != handedBack) gZoom = std::clamp(zoomMetres <= 0 ? 800 : zoomMetres, 25, 8000);

    MSG msg;
    while (PeekMessage(&msg, gWnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (gWantClose) {
        want = false;
        zoomMetres = handedBack = gZoom;
        destroy();
        return true;
    }

    RECT rc;
    GetClientRect(gWnd, &rc);
    const int nw = std::max<LONG>(1, rc.right), nh = std::max<LONG>(1, rc.bottom);
    if (nw != gW || nh != gH) {
        releaseRtv();
        if (SUCCEEDED(gSwap->ResizeBuffers(0, UINT(nw), UINT(nh), DXGI_FORMAT_UNKNOWN, 0))) {
            gW = nw;
            gH = nh;
            makeRtv();
        }
    }
    if (!gRtv) {
        zoomMetres = handedBack = gZoom;
        return true;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (frame.ok) {
        const Counts n = countDots(frame, cfg, gSideOpen);
        drawRadar(frame, cfg, n);
        if (gSideOpen) drawSidebar(cfg, n, true, frame.online);
    } else {
        drawWaiting();
    }
    ImGui::Render();

    const float clear[4] = {13 / 255.f, 15 / 255.f, 19 / 255.f, 1.f};
    gCtx->OMSetRenderTargets(1, gRtv.GetAddressOf(), nullptr);
    gCtx->ClearRenderTargetView(gRtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    const HRESULT hr = gSwap->Present(0, 0);
    zoomMetres = handedBack = gZoom;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        rlog("[!] radar: device lost; recreating next tick");
        destroy();
    }
    return true;
}

}
