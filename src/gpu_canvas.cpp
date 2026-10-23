
#include "gpu_canvas.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

D2D1_COLOR_F toColor(COLORREF c) {
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, 1.f);
}

std::string hrText(const char* what, HRESULT hr) {
    char b[96];
    std::snprintf(b, sizeof b, "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return b;
}

class D2dRenderer final : public GpuRenderer, public Canvas {
public:
    bool init(HWND hwnd, std::string& err) {
        hwnd_ = hwnd;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                       ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d_, nullptr, &ctx_);
        if (FAILED(hr)) return err = hrText("D3D11CreateDevice", hr), false;

        ComPtr<IDXGIDevice1> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(hr = d3d_.As(&dxgiDev))) return err = hrText("IDXGIDevice1", hr), false;
        if (FAILED(hr = dxgiDev->GetAdapter(&adapter))) return err = hrText("GetAdapter", hr), false;
        if (FAILED(hr = adapter->GetParent(IID_PPV_ARGS(&factory)))) return err = hrText("IDXGIFactory2", hr), false;

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 3;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGIFactory5> f5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory.As(&f5)) && SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)))
            tearing_ = tearing != FALSE;
        scFlags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        sd.Flags = scFlags_;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(hr = factory->CreateSwapChainForHwnd(d3d_.Get(), hwnd, &sd, nullptr, nullptr, &sc1)))
            return err = hrText("CreateSwapChainForHwnd", hr), false;
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(hr = sc1.As(&sc_))) return err = hrText("IDXGISwapChain2", hr), false;
        sc_->SetMaximumFrameLatency(1);
        waitable_ = sc_->GetFrameLatencyWaitableObject();
        dxgiDev->SetMaximumFrameLatency(1);

        D2D1_FACTORY_OPTIONS fo{};
        if (FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                                          reinterpret_cast<void**>(d2dFactory_.GetAddressOf()))))
            return err = hrText("D2D1CreateFactory", hr), false;
        ComPtr<ID2D1Device> d2dDev;
        if (FAILED(hr = d2dFactory_->CreateDevice(dxgiDev.Get(), &d2dDev))) return err = hrText("ID2D1Factory1::CreateDevice", hr), false;
        if (FAILED(hr = d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)))
            return err = hrText("CreateDeviceContext", hr), false;
        dc_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
        if (FAILED(hr = dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_))) return err = hrText("CreateSolidColorBrush", hr), false;
        const D2D1_STROKE_STYLE_PROPERTIES1 ssp = D2D1::StrokeStyleProperties1(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                               D2D1_LINE_JOIN_ROUND);
        if (FAILED(hr = d2dFactory_->CreateStrokeStyle(ssp, nullptr, 0, &round_))) return err = hrText("CreateStrokeStyle", hr), false;

        ComPtr<IDWriteFactory>& dw = dw_;
        if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return err = hrText("DWriteCreateFactory", hr), false;
        if (FAILED(hr = dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             13.f, L"en-us", &font_)))
            return err = hrText("CreateTextFormat", hr), false;
        font_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        if (!bindBackBuffer(err)) return false;
        DXGI_ADAPTER_DESC ad{};
        adapter->GetDesc(&ad);
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name - 1, nullptr, nullptr);
        desc_ = std::string("D3D11 flip + Direct2D on ") + name + " (" + std::to_string(w_) + "x" + std::to_string(h_) +
                (tearing_ ? ", tearing ok)" : ", no tearing)");
        return true;
    }

    ~D2dRenderer() override {
        if (dc_) dc_->SetTarget(nullptr);
        if (waitable_) CloseHandle(waitable_);
    }

    const std::string& describe() const override { return desc_; }

    void waitForFrame() override {
        if (waitable_) WaitForSingleObjectEx(waitable_, 100, TRUE);
    }

    Canvas* begin(COLORREF clear) override {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        if (rc.right > 0 && rc.bottom > 0 && (rc.right != w_ || rc.bottom != h_)) {
            dc_->SetTarget(nullptr);
            target_.Reset();
            if (FAILED(sc_->ResizeBuffers(0, UINT(rc.right), UINT(rc.bottom), DXGI_FORMAT_UNKNOWN, scFlags_)))
                return nullptr;
            std::string err;
            if (!bindBackBuffer(err)) return nullptr;
        }
        dc_->BeginDraw();
        dc_->Clear(toColor(clear));
        penColor_ = RGB(255, 255, 255);
        penWidth_ = 1;
        return this;
    }

    bool end() override { return SUCCEEDED(dc_->EndDraw()); }

    bool present(bool vsync) override {
        if (!vsync) {
            const HRESULT nh = sc_->Present(0, DXGI_PRESENT_DO_NOT_WAIT | (tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0));
            if (nh == DXGI_STATUS_OCCLUDED) Sleep(8);
            return nh == DXGI_ERROR_WAS_STILL_DRAWING || nh == DXGI_STATUS_OCCLUDED || SUCCEEDED(nh);
        }
        const HRESULT hr = sc_->Present(1, 0);
        if (hr == DXGI_STATUS_OCCLUDED) Sleep(8);
        return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET && SUCCEEDED(hr);
    }

    void releaseImages() override { images_.clear(); }

    void pen(COLORREF c, int width) override {
        penColor_ = c;
        penWidth_ = width < 1 ? 1 : width;
    }
    void line(int x0, int y0, int x1, int y1) override {
        stroke();
        dc_->DrawLine(D2D1::Point2F(x0 + .5f, y0 + .5f), D2D1::Point2F(x1 + .5f, y1 + .5f), brush_.Get(), float(penWidth_), round_.Get());
    }
    void rect(int l, int t, int r, int b) override {
        stroke();
        dc_->DrawRectangle(D2D1::RectF(l + .5f, t + .5f, r - .5f, b - .5f), brush_.Get(), float(penWidth_), round_.Get());
    }
    void ellipse(int l, int t, int r, int b) override {
        stroke();
        const float rx = (r - l - 1) / 2.f, ry = (b - t - 1) / 2.f;
        dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(l + .5f + rx, t + .5f + ry), rx, ry), brush_.Get(), float(penWidth_), round_.Get());
    }
    void roundRect(int l, int t, int r, int b, int cw, int ch) override {
        stroke();
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l + .5f, t + .5f, r - .5f, b - .5f), cw / 2.f, ch / 2.f), brush_.Get(),
                                  float(penWidth_), round_.Get());
    }
    void polyline(const POINT* p, int n) override {
        for (int i = 0; i + 1 < n; ++i) line(p[i].x, p[i].y, p[i + 1].x, p[i + 1].y);
    }
    void fill(const RECT& r, COLORREF c) override {
        brush_->SetColor(toColor(c));
        dc_->FillRectangle(D2D1::RectF(float(r.left), float(r.top), float(r.right), float(r.bottom)), brush_.Get());
    }
    void text(const char* s, const RECT& r, COLORREF c, UINT dt) override {
        const Layout* lay = layout(s);
        if (!lay) return;
        const float x = (dt & DT_CENTER) ? (r.left + r.right) / 2.f - lay->w / 2.f : float(r.left);
        const float y = (dt & DT_BOTTOM) ? float(r.bottom) - lay->h : float(r.top);
        brush_->SetColor(toColor(c));
        dc_->DrawTextLayout(D2D1::Point2F(std::floor(x), std::floor(y)), lay->l.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    bool image(const std::string& path, float u0, float v0, float u1, float v1, const RECT& dst, float opacity) override {
        ID2D1Bitmap* bmp = bitmap(path);
        if (!bmp) return false;
        const D2D1_SIZE_F sz = bmp->GetSize();
        dc_->DrawBitmap(bmp, D2D1::RectF(float(dst.left), float(dst.top), float(dst.right), float(dst.bottom)), opacity,
                        D2D1_INTERPOLATION_MODE_LINEAR, D2D1::RectF(u0 * sz.width, v0 * sz.height, u1 * sz.width, v1 * sz.height));
        return true;
    }

private:
    struct Decoded {
        std::mutex mx;
        bool done = false, ok = false;
        std::vector<uint8_t> px;
        UINT w = 0, h = 0;
    };
    struct Image {
        std::shared_ptr<Decoded> pending;
        ComPtr<ID2D1Bitmap> bmp;
        UINT rows = 0;
        bool failed = false;
    };

    static void decodeFile(const std::string& path, Decoded& d) {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = false;
        std::vector<uint8_t> px;
        UINT w = 0, h = 0;
        {
            ComPtr<IWICImagingFactory> wic;
            ComPtr<IWICBitmapDecoder> dec;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> conv;
            wchar_t wpath[MAX_PATH];
            if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, MAX_PATH) > 0 &&
                SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                SUCCEEDED(wic->CreateDecoderFromFilename(wpath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
                SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h) {
                ComPtr<IWICBitmapSource> src = frame;
                constexpr UINT kMax = 8192;
                if (w > kMax || h > kMax) {
                    ComPtr<IWICBitmapScaler> sc;
                    const double k = double(kMax) / double(w > h ? w : h);
                    w = UINT(w * k);
                    h = UINT(h * k);
                    if (SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant)))
                        src = sc;
                    else
                        w = h = 0;
                }
                if (w && h && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                    SUCCEEDED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeCustom))) {
                    try {
                        px.resize(size_t(w) * h * 4);
                        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()));
                    } catch (const std::bad_alloc&) {
                        ok = false;
                    }
                }
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
        std::lock_guard<std::mutex> lk(d.mx);
        d.ok = ok;
        if (ok) {
            d.px = std::move(px);
            d.w = w;
            d.h = h;
        }
        d.done = true;
    }

    ID2D1Bitmap* bitmap(const std::string& path) {
        const auto it = images_.find(path);
        if (it == images_.end()) {
            Image& im = images_[path];
            im.pending = std::make_shared<Decoded>();
            try {
                std::thread([path, d = im.pending] { decodeFile(path, *d); }).detach();
            } catch (...) {
                im.pending.reset();
                im.failed = true;
            }
            return nullptr;
        }
        Image& im = it->second;
        if (im.failed) return nullptr;
        if (!im.pending) return im.bmp.Get();
        const std::shared_ptr<Decoded> d = im.pending;
        std::lock_guard<std::mutex> lk(d->mx);
        if (!d->done) return nullptr;
        const auto fail = [&im] {
            im.pending.reset();
            im.bmp.Reset();
            im.failed = true;
            return nullptr;
        };
        if (!d->ok) return fail();
        if (!im.bmp) {
            const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(dc_->CreateBitmap(D2D1::SizeU(d->w, d->h), nullptr, 0, bp, im.bmp.ReleaseAndGetAddressOf()))) return fail();
        }
        constexpr UINT kRowsPerFrame = 512;
        const UINT n = (std::min)(kRowsPerFrame, d->h - im.rows);
        const D2D1_RECT_U r = D2D1::RectU(0, im.rows, d->w, im.rows + n);
        if (FAILED(im.bmp->CopyFromMemory(&r, d->px.data() + size_t(im.rows) * d->w * 4, d->w * 4))) return fail();
        im.rows += n;
        if (im.rows < d->h) return nullptr;
        im.pending.reset();
        return im.bmp.Get();
    }

    struct Layout {
        ComPtr<IDWriteTextLayout> l;
        float w = 0, h = 0;
    };

    const Layout* layout(const char* s) {
        const auto it = texts_.find(s);
        if (it != texts_.end()) return it->second.l ? &it->second : nullptr;
        if (texts_.size() >= 4096) texts_.clear();
        Layout& lay = texts_[s];
        wchar_t w[256];
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, 255);
        if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, s, -1, w, 255);
        if (n <= 1 || FAILED(dw_->CreateTextLayout(w, UINT32(n - 1), font_.Get(), 8192.f, 8192.f, &lay.l))) return nullptr;
        DWRITE_TEXT_METRICS m{};
        lay.l->GetMetrics(&m);
        lay.w = m.widthIncludingTrailingWhitespace;
        lay.h = m.height;
        return &lay;
    }

    void stroke() { brush_->SetColor(toColor(penColor_)); }

    bool bindBackBuffer(std::string& err) {
        ComPtr<IDXGISurface> surf;
        HRESULT hr = sc_->GetBuffer(0, IID_PPV_ARGS(&surf));
        if (FAILED(hr)) return err = hrText("GetBuffer", hr), false;
        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
        if (FAILED(hr = dc_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target_))) return err = hrText("CreateBitmapFromDxgiSurface", hr), false;
        dc_->SetTarget(target_.Get());
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sc_->GetDesc1(&sd);
        w_ = int(sd.Width);
        h_ = int(sd.Height);
        return true;
    }

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain2> sc_;
    HANDLE waitable_ = nullptr;
    bool tearing_ = false;
    UINT scFlags_ = 0;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle1> round_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<IDWriteTextFormat> font_;
    std::unordered_map<std::string, Layout> texts_;
    std::map<std::string, Image> images_;
    COLORREF penColor_ = RGB(255, 255, 255);
    int penWidth_ = 1;
    int w_ = 0, h_ = 0;
    std::string desc_;
};

}

std::unique_ptr<GpuRenderer> GpuRenderer::create(HWND hwnd, std::string& err) {
    auto r = std::make_unique<D2dRenderer>();
    if (!r->init(hwnd, err)) return nullptr;
    return r;
}
