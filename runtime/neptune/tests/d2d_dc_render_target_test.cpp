#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <algorithm>

using namespace D2D1;

template<class T> static void release(T *&p) { if (p) { p->Release(); p = nullptr; } }
static bool report(const char *name, HRESULT hr) {
    std::printf("%-40s HRESULT 0x%08lx\n", name, (unsigned long)hr);
    return SUCCEEDED(hr);
}

struct Dib {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t *pixels = nullptr;
    int width = 0, height = 0;
    bool create(int w, int h) {
        width = w; height = h;
        dc = CreateCompatibleDC(nullptr);
        std::printf("CreateCompatibleDC: %s (error %lu)\n", dc ? "OK" : "FAIL", GetLastError());
        if (!dc) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&pixels, nullptr, 0);
        std::printf("CreateDIBSection: %s (error %lu)\n", bitmap && pixels ? "OK" : "FAIL", GetLastError());
        if (!bitmap || !pixels) return false;
        old = SelectObject(dc, bitmap);
        if (!old || old == HGDI_ERROR) { old = nullptr; std::printf("SelectObject FAIL\n"); return false; }
        std::fill(pixels, pixels + w * h, 0xff112233u);
        return true;
    }
    ~Dib() {
        if (old) SelectObject(dc, old);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};

static bool rgb(uint32_t p, uint32_t expected) {
    for (int shift = 0; shift < 24; shift += 8)
        if (std::abs(int((p >> shift) & 255) - int((expected >> shift) & 255)) > 2) return false;
    return true;
}
static bool verify(Dib &d, RECT rc, bool full) {
    unsigned exterior = 0, clear = 0, fill = 0, bitmap = 0, text = 0;
    uint32_t bg = full ? 0x00104080 : 0x00508020;
    for (int y = 0; y < d.height; ++y) for (int x = 0; x < d.width; ++x) {
        uint32_t p = d.pixels[y * d.width + x];
        if (x < rc.left || x >= rc.right || y < rc.top || y >= rc.bottom) {
            if (p != 0xff112233u) ++exterior;
            continue;
        }
        int u = x - rc.left, v = y - rc.top;
        if (u >= 10 && u < 60 && v >= 10 && v < 50) {
            if (!rgb(p, full ? 0x00e03020 : 0x00d060a0)) ++fill;
        } else if (full && u >= 80 && u < 112 && v >= 10 && v < 42) {
            if (!rgb(p, 0x0020c030)) ++bitmap;
        } else if (full && u >= 10 && u < 150 && v >= 70 && v < 120) {
            if (!rgb(p, bg)) ++text;
        } else if (!rgb(p, bg)) ++clear;
    }
    std::printf("pixel checks: exterior_bad=%u clear_bad=%u fill_bad=%u bitmap_bad=%u text_changed=%u\n",
                exterior, clear, fill, bitmap, text);
    if (full && !text) std::printf("FAIL: no text pixels; check whether a system font is installed\n");
    return !exterior && !clear && !fill && !bitmap && (!full || text);
}

static bool run_draw(ID2D1DCRenderTarget *rt, Dib &dib, RECT rc, bool full,
                     ID2D1Bitmap *bitmap, IDWriteTextLayout *layout) {
    HRESULT hr = rt->BindDC(dib.dc, &rc);
    if (!report("BindDC", hr)) return false;
    ID2D1SolidColorBrush *brush = nullptr, *white = nullptr;
    ColorF color = full ? ColorF(0xe03020) : ColorF(0xd060a0);
    hr = rt->CreateSolidColorBrush(color, &brush);
    if (!report("CreateSolidColorBrush(fill)", hr)) return false;
    if (full) {
        hr = rt->CreateSolidColorBrush(ColorF(0xffffff), &white);
        if (!report("CreateSolidColorBrush(text)", hr)) { release(brush); return false; }
    }
    rt->BeginDraw();
    rt->Clear(full ? ColorF(0x104080) : ColorF(0x508020));
    rt->FillRectangle(RectF(10, 10, 60, 50), brush);
    if (full) {
        rt->DrawBitmap(bitmap, RectF(80, 10, 112, 42), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        rt->DrawTextLayout(Point2F(10, 70), layout, white);
    }
    hr = rt->EndDraw();
    bool ok = report("EndDraw", hr);
    BOOL flushed = GdiFlush();
    std::printf("GdiFlush: %s\n", flushed ? "OK" : "FAIL");
    ok = ok && flushed && verify(dib, rc, full);
    release(white); release(brush);
    return ok;
}

int main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!report("CoInitializeEx", hr)) return 1;
    ID2D1Factory *factory = nullptr;
    ID2D1DCRenderTarget *rt = nullptr, *probe = nullptr;
    IWICImagingFactory *wic = nullptr;
    IWICBitmap *wic_bitmap = nullptr;
    ID2D1Bitmap *bitmap = nullptr;
    IDWriteFactory *write = nullptr;
    IDWriteTextFormat *format = nullptr;
    IDWriteTextLayout *layout = nullptr;
    bool ok = false;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
    if (!report("D2D1CreateFactory", hr)) goto done;
    {
        D2D1_RENDER_TARGET_PROPERTIES p = RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96,
            D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        hr = factory->CreateDCRenderTarget(&p, &rt);
        if (!report("CreateDCRenderTarget IGNORE/NONE", hr)) goto done;
        p.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        hr = factory->CreateDCRenderTarget(&p, &probe);
        report("CreateDCRenderTarget PREMULTIPLIED", hr); release(probe);
        p.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        p.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;
        hr = factory->CreateDCRenderTarget(&p, &probe);
        report("CreateDCRenderTarget GDI_COMPATIBLE", hr); release(probe);
    }
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWICImagingFactory, (void **)&wic);
    if (!report("CoCreateInstance WIC factory", hr)) goto done;
    {
        uint32_t pixels[16 * 16];
        std::fill(pixels, pixels + 16 * 16, 0xff20c030u);
        hr = wic->CreateBitmapFromMemory(16, 16, GUID_WICPixelFormat32bppPBGRA,
                                         16 * 4, sizeof(pixels), (BYTE *)pixels, &wic_bitmap);
        if (!report("WIC CreateBitmapFromMemory", hr)) goto done;
        hr = rt->CreateBitmapFromWicBitmap(wic_bitmap, nullptr, &bitmap);
        if (!report("CreateBitmapFromWicBitmap", hr)) goto done;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&write);
    if (!report("DWriteCreateFactory", hr)) goto done;
    hr = write->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"en-us", &format);
    if (!report("CreateTextFormat (Arial)", hr)) goto done;
    hr = write->CreateTextLayout(L"TEST", 4, format, 140, 50, &layout);
    if (!report("CreateTextLayout", hr)) goto done;
    {
        Dib first, second;
        if (!first.create(256, 192)) goto done;
        bool first_ok = run_draw(rt, first, RECT{24, 16, 224, 176}, true, bitmap, layout);
        if (!second.create(320, 240)) goto done;
        bool second_ok = run_draw(rt, second, RECT{10, 20, 310, 220}, false, nullptr, nullptr);
        ok = first_ok && second_ok;
    }
done:
    release(layout); release(format); release(write); release(bitmap); release(wic_bitmap);
    release(wic); release(rt); release(factory);
    CoUninitialize();
    std::printf("D2D DC render target: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
