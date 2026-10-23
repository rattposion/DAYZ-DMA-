#pragma once

#include <windows.h>

#include <memory>
#include <string>

struct Canvas {
    virtual ~Canvas() = default;
    virtual void pen(COLORREF c, int width) = 0;
    virtual void line(int x0, int y0, int x1, int y1) = 0;
    virtual void rect(int l, int t, int r, int b) = 0;
    virtual void ellipse(int l, int t, int r, int b) = 0;
    virtual void roundRect(int l, int t, int r, int b, int cw, int ch) = 0;
    virtual void polyline(const POINT* p, int n) = 0;
    virtual void fill(const RECT& r, COLORREF c) = 0;
    virtual void text(const char* s, const RECT& r, COLORREF c, UINT dt) = 0;
    virtual bool image(const std::string& path, float u0, float v0, float u1, float v1, const RECT& dst, float opacity) {
        (void)path, (void)u0, (void)v0, (void)u1, (void)v1, (void)dst, (void)opacity;
        return false;
    }
};

class GpuRenderer {
public:
    static std::unique_ptr<GpuRenderer> create(HWND hwnd, std::string& err);
    virtual ~GpuRenderer() = default;
    virtual const std::string& describe() const = 0;
    virtual void waitForFrame() = 0;
    virtual Canvas* begin(COLORREF clear) = 0;
    virtual bool end() = 0;
    virtual bool present(bool vsync = true) = 0;
    virtual void releaseImages() = 0;
};
