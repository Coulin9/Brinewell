#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

struct Check { const char *name; bool pass; HRESULT hr; };
static std::vector<Check> checks;
static bool record(const char *name, HRESULT hr, bool pass) {
    std::printf("%-40s %s HRESULT 0x%08lx\n", name, pass ? "PASS" : "FAIL", (unsigned long)hr);
    checks.push_back({name, pass, hr});
    return pass;
}
static bool result(const char *name, HRESULT hr) { return record(name, hr, SUCCEEDED(hr)); }
template<typename T> struct Ptr {
    T *p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    T **out() { return &p; }
};
template<typename T, typename U> static HRESULT qi(U *source, Ptr<T> &dest) {
    return source->QueryInterface(__uuidof(T), (void **)dest.out());
}
static void multithread(const char *name, IUnknown *source) {
    Ptr<ID3D11Multithread> mt;
    HRESULT hr = qi(source, mt);
    if (!result(name, hr)) return;
    BOOL previous = mt->SetMultithreadProtected(TRUE);
    BOOL current = mt->GetMultithreadProtected();
    std::printf("  SetMultithreadProtected previous=%d, Get=%d\n", previous, current);
    mt->Enter(); mt->Leave();
    record("Multithread protection enabled", S_OK, current != FALSE);
    mt->SetMultithreadProtected(previous);
}
static bool readback(ID3D11Device *device, ID3D11DeviceContext *context,
                     ID3D11Texture2D *source, ID3D11Texture2D *staging,
                     const char *label) {
    context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (!result(label, hr)) return false;
    unsigned bad = 0;
    for (unsigned y = 0; y < 64; ++y) {
        const uint8_t *row = static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch;
        for (unsigned x = 0; x < 64; ++x) {
            const uint8_t *p = row + x * 4;
            if (p[0] != 0xc0 || p[1] != 0x80 || p[2] != 0x40 || p[3] != 0xff) ++bad;
        }
    }
    std::printf("  row pitch=%u bad pixels=%u first BGRA=%02x %02x %02x %02x\n",
                mapped.RowPitch, bad, ((uint8_t *)mapped.pData)[0], ((uint8_t *)mapped.pData)[1],
                ((uint8_t *)mapped.pData)[2], ((uint8_t *)mapped.pData)[3]);
    context->Unmap(staging, 0);
    HRESULT removed = device->GetDeviceRemovedReason();
    result("GetDeviceRemovedReason", removed);
    return record(std::strcmp(label, "GDI staging Map") == 0 ? "GDI readback pixels" : "Core readback pixels",
                  removed, bad == 0 && SUCCEEDED(removed));
}
static void draw(ID3D11DeviceContext *context, ID3D11RenderTargetView *view,
                 ID3D11VertexShader *vs, ID3D11PixelShader *ps) {
    const float clear[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(view, clear);
    context->OMSetRenderTargets(1, &view, nullptr);
    D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
    context->RSSetViewports(1, &vp);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
}
static bool compile(const char *source, const char *entry, const char *profile, Ptr<ID3DBlob> &blob) {
    Ptr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                            entry, profile, 0, 0, blob.out(), errors.out());
    if (errors) std::printf("  compiler: %.*s\n", (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
    return result(profile, hr);
}
static void plain_surface_probes(ID3D11Texture2D *texture, ID3D11Device *device) {
    Ptr<IDXGISurface> surface;
    Ptr<IDXGISurface1> surface1;
    Ptr<IDXGISurface2> surface2;
    HRESULT hr = qi(texture, surface);
    result("Plain QI IDXGISurface", hr);
    hr = qi(texture, surface1);
    result("Plain QI IDXGISurface1", hr);
    hr = qi(texture, surface2);
    result("Plain QI IDXGISurface2", hr);
    if (surface) {
        DXGI_SURFACE_DESC desc{};
        hr = surface->GetDesc(&desc);
        record("Plain surface GetDesc", hr, SUCCEEDED(hr) && desc.Width == 64 &&
               desc.Height == 64 && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
               desc.SampleDesc.Count == 1);
        Ptr<ID3D11Device> owner;
        hr = surface->GetDevice(__uuidof(ID3D11Device), (void **)owner.out());
        record("Plain surface GetDevice", hr, SUCCEEDED(hr) && owner.p == device);
        Ptr<ID3D11Device> parent;
        hr = surface->GetParent(__uuidof(ID3D11Device), (void **)parent.out());
        record("Plain surface GetParent", hr, SUCCEEDED(hr) && parent.p == device);
    }
    if (surface1) {
        HDC dc = nullptr;
        hr = surface1->GetDC(FALSE, &dc);
        record("Plain surface GetDC rejected", hr, FAILED(hr) && dc == nullptr);
        if (SUCCEEDED(hr)) surface1->ReleaseDC(nullptr);
    }
    if (surface2) {
        Ptr<ID3D11Texture2D> parent;
        UINT subresource = ~0u;
        hr = surface2->GetResource(__uuidof(ID3D11Texture2D), (void **)parent.out(), &subresource);
        record("Surface2 GetResource", hr, SUCCEEDED(hr) && parent.p == texture && subresource == 0);
    }
}
static void ineligible_surface_probes(ID3D11Device *device, D3D11_TEXTURE2D_DESC desc,
                                      const char *label) {
    Ptr<ID3D11Texture2D> texture;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, texture.out());
    if (!result(label, hr)) return;
    Ptr<IDXGISurface> s;
    Ptr<IDXGISurface1> s1;
    Ptr<IDXGISurface2> s2;
    hr = qi(texture.p, s);
    record("Ineligible QI surface", hr, hr == E_NOINTERFACE && !s);
    hr = qi(texture.p, s1);
    record("Ineligible QI surface1", hr, hr == E_NOINTERFACE && !s1);
    hr = qi(texture.p, s2);
    record("Ineligible QI surface2", hr, hr == E_NOINTERFACE && !s2);
}
static void staging_surface_probes(ID3D11Texture2D *texture) {
    Ptr<IDXGISurface> surface;
    HRESULT hr = qi(texture, surface);
    if (!result("Staging QI IDXGISurface", hr)) return;
    hr = surface->Unmap();
    record("Unbalanced surface Unmap", hr, hr == DXGI_ERROR_INVALID_CALL);
    DXGI_MAPPED_RECT rect{};
    hr = surface->Map(&rect, 0);
    record("Staging zero flags rejected", hr, hr == E_INVALIDARG);
    hr = surface->Map(&rect, DXGI_MAP_DISCARD);
    record("Staging discard-only rejected", hr, hr == E_INVALIDARG);
    hr = surface->Map(&rect, DXGI_MAP_WRITE | DXGI_MAP_DISCARD);
    record("Staging discard rejected", hr, hr == E_INVALIDARG);
    hr = surface->Map(&rect, DXGI_MAP_WRITE);
    if (!result("Staging surface Map WRITE", hr)) return;
    bool pitch_ok = rect.pBits && rect.Pitch >= 64 * 4;
    record("Staging write pitch", S_OK, pitch_ok);
    DXGI_MAPPED_RECT second{};
    HRESULT double_hr = surface->Map(&second, DXGI_MAP_WRITE);
    record("Staging double Map rejected", double_hr, FAILED(double_hr));
    if (pitch_ok) {
        for (unsigned y = 0; y < 64; ++y)
            for (unsigned x = 0; x < 64 * 4; ++x)
                rect.pBits[y * rect.Pitch + x] = static_cast<BYTE>((x + y) & 255);
    }
    result("Staging surface Unmap WRITE", surface->Unmap());
    rect = {};
    hr = surface->Map(&rect, DXGI_MAP_READ);
    if (!result("Staging surface Map READ", hr)) return;
    bool valid = rect.pBits && rect.Pitch >= 64 * 4;
    if (valid) {
        for (unsigned y = 0; y < 64; ++y)
            for (unsigned x = 0; x < 64 * 4; ++x)
                valid &= rect.pBits[y * rect.Pitch + x] == static_cast<BYTE>((x + y) & 255);
    }
    record("Staging surface pattern", S_OK, valid);
    result("Staging surface Unmap READ", surface->Unmap());
}
static void surface_probes(ID3D11Texture2D *texture, ID3D11Device *device) {
    Ptr<IDXGISurface> surface;
    Ptr<IDXGISurface1> surface1;
    Ptr<IDXGISurface2> surface2;
    HRESULT hr = qi(texture, surface2);
    result("GDI QI IDXGISurface2", hr);
    hr = qi(texture, surface);
    if (result("GDI QI IDXGISurface", hr)) {
        Ptr<ID3D11Device> surface_device;
        hr = surface->GetDevice(__uuidof(ID3D11Device), (void **)surface_device.out());
        if (result("IDXGISurface GetDevice", hr)) {
            Ptr<IUnknown> a, b;
            HRESULT h1 = qi(device, a), h2 = qi(surface_device.p, b);
            record("Surface device identity", FAILED(h1) ? h1 : h2,
                   SUCCEEDED(h1) && SUCCEEDED(h2) && a.p == b.p);
        }
    }
    hr = qi(texture, surface1);
    if (!result("GDI QI IDXGISurface1", hr)) return;
}
static bool gdi_readback(ID3D11DeviceContext *ctx, ID3D11Texture2D *source,
                         ID3D11Texture2D *staging, bool partial, bool solid,
                         const char *label) {
    ctx->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (!result(label, hr)) return false;
    bool good = mapped.pData && mapped.RowPitch >= 64 * 4;
    if (good) {
        for (unsigned y = 0; y < 64; ++y) {
            const auto *row = static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch;
            for (unsigned x = 0; x < 64; ++x) {
                const bool b = solid || (partial && x < 20 && y >= 8 && y < 24);
                const auto *p = row + x * 4;
                good &= p[0] == (b ? 0x30 : 0xc0) && p[1] == (b ? 0x20 : 0x80) &&
                        p[2] == (b ? 0x10 : 0x40);
            }
        }
    }
    ctx->Unmap(staging, 0);
    return record("GDI expected RGB pixels", S_OK, good);
}
static void gdi_roundtrip(ID3D11Texture2D *texture, ID3D11Texture2D *staging,
                          ID3D11DeviceContext *ctx, ID3D11RenderTargetView *gdi_view,
                          ID3D11RenderTargetView *other_view) {
    Ptr<IDXGISurface1> surface;
    HRESULT hr = qi(texture, surface);
    if (!result("GDI roundtrip QI", hr)) return;
    hr = surface->ReleaseDC(nullptr);
    record("GDI unbalanced ReleaseDC", hr, hr == DXGI_ERROR_INVALID_CALL);
    ID3D11RenderTargetView *views[2] = {gdi_view, other_view};
    ctx->OMSetRenderTargets(2, views, nullptr);
    HDC dc = nullptr;
    hr = surface->GetDC(FALSE, &dc);
    if (!record("GDI GetDC preserve", hr, SUCCEEDED(hr) && dc)) return;
    ID3D11RenderTargetView *bound[2]{};
    ctx->OMGetRenderTargets(2, bound, nullptr);
    record("GDI OM unbind only target", S_OK, !bound[0] && bound[1] == other_view);
    for (auto *v : bound) if (v) v->Release();
    record("GDI readback GetPixel", S_OK, GetPixel(dc, 4, 4) == RGB(0x40, 0x80, 0xc0));
    HDC second = nullptr;
    hr = surface->GetDC(FALSE, &second);
    record("GDI double GetDC rejected", hr, FAILED(hr) && !second);
    DXGI_MAPPED_RECT map{};
    hr = surface->Map(&map, DXGI_MAP_READ);
    record("GDI Map while DC rejected", hr, FAILED(hr));
    HBRUSH brush = CreateSolidBrush(RGB(0x10, 0x20, 0x30));
    RECT paint{0, 8, 20, 24};
    bool painted = brush && FillRect(dc, &paint, brush);
    if (brush) DeleteObject(brush);
    record("GDI fill partial rectangle", S_OK, painted);
    RECT clipped{-5, 8, 20, 24};
    result("GDI ReleaseDC clipped", surface->ReleaseDC(&clipped));
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    gdi_readback(ctx, texture, staging, true, false, "GDI clipped readback Map");
    dc = nullptr;
    hr = surface->GetDC(TRUE, &dc);
    if (!record("GDI GetDC discard", hr, SUCCEEDED(hr) && dc)) return;
    SetPixel(dc, 1, 1, RGB(0xff, 0, 0));
    RECT empty{10, 10, 10, 10};
    result("GDI ReleaseDC empty", surface->ReleaseDC(&empty));
    gdi_readback(ctx, texture, staging, true, false, "GDI empty readback Map");
    dc = nullptr;
    hr = surface->GetDC(TRUE, &dc);
    if (!record("GDI full GetDC discard", hr, SUCCEEDED(hr) && dc)) return;
    brush = CreateSolidBrush(RGB(0x10, 0x20, 0x30));
    RECT full{0, 0, 64, 64};
    painted = brush && FillRect(dc, &full, brush);
    if (brush) DeleteObject(brush);
    record("GDI fill full rectangle", S_OK, painted);
    result("GDI ReleaseDC full", surface->ReleaseDC(nullptr));
    gdi_readback(ctx, texture, staging, false, true, "GDI full readback Map");
    for (unsigned i = 0; i < 8; ++i) {
        dc = nullptr;
        hr = surface->GetDC(FALSE, &dc);
        if (!record("GDI repeated GetDC", hr, SUCCEEDED(hr) && dc)) break;
        hr = surface->ReleaseDC(nullptr);
        if (!result("GDI repeated ReleaseDC", hr)) break;
    }
}
int main() {
    bool core = true;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_10_0, obtained = (D3D_FEATURE_LEVEL)0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, &requested, 1, D3D11_SDK_VERSION,
        device.out(), &obtained, context.out());
    if (!result("D3D11CreateDevice FL10 BGRA", hr)) { core = false; goto summary; }
    std::printf("  returned feature level=0x%04x\n", (unsigned)obtained);
    core &= record("Exact feature level 10_0", S_OK, obtained == D3D_FEATURE_LEVEL_10_0);
    {
        Ptr<ID3D11Device1> device1, dxgi_device1;
        Ptr<ID3D11DeviceContext1> context1;
        Ptr<IDXGIDevice> dxgi;
        Ptr<IDXGIDevice1> dxgi1;
        hr = qi(device.p, device1); result("Device QI ID3D11Device1", hr);
        hr = qi(context.p, context1); result("Context QI ID3D11DeviceContext1", hr);
        multithread("Device QI ID3D11Multithread", device.p);
        multithread("Context QI ID3D11Multithread", context.p);
        hr = qi(device.p, dxgi1); result("Device QI IDXGIDevice1", hr);
        hr = qi(device.p, dxgi);
        if (result("Device QI IDXGIDevice", hr)) {
            hr = qi(dxgi.p, dxgi_device1); result("DXGI QI ID3D11Device1", hr);
            Ptr<IUnknown> a, b, c;
            HRESULT h1 = qi(device.p, a), h2 = qi(dxgi.p, b);
            HRESULT h3 = dxgi_device1 ? qi(dxgi_device1.p, c) : E_NOINTERFACE;
            record("COM identity Device/DXGI/Device1", FAILED(h1) ? h1 : FAILED(h2) ? h2 : h3,
                   SUCCEEDED(h1) && SUCCEEDED(h2) && SUCCEEDED(h3) && a.p == b.p && a.p == c.p);
            Ptr<IDXGIAdapter> adapter;
            hr = dxgi->GetAdapter(adapter.out());
            if (result("IDXGIDevice GetAdapter", hr)) {
                Ptr<IDXGIFactory> factory;
                Ptr<IDXGIFactory1> factory1;
                hr = adapter->GetParent(__uuidof(IDXGIFactory), (void **)factory.out());
                result("Adapter GetParent IDXGIFactory", hr);
                hr = adapter->GetParent(__uuidof(IDXGIFactory1), (void **)factory1.out());
                result("Adapter GetParent IDXGIFactory1", hr);
            }
        }
    }
    {
        UINT support = 0;
        hr = device->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &support);
        constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET |
                                  D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
        std::printf("BGRA support mask=0x%08x TEXTURE2D=%d RENDER_TARGET=%d SHADER_SAMPLE=%d DISPLAY=%d\n",
            support, !!(support & D3D11_FORMAT_SUPPORT_TEXTURE2D), !!(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET),
            !!(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE), !!(support & D3D11_FORMAT_SUPPORT_DISPLAY));
        core &= record("BGRA format support", hr, SUCCEEDED(hr) && (support & required) == required);
    }
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Ptr<ID3D11Texture2D> texture, staging, gdi;
        Ptr<ID3D11RenderTargetView> view, gdi_view;
        hr = device->CreateTexture2D(&desc, nullptr, texture.out());
        bool render_ok = result("Create render target texture", hr);
        if (render_ok) {
            plain_surface_probes(texture.p, device.p);
            hr = device->CreateRenderTargetView(texture.p, nullptr, view.out());
            render_ok = result("Create render target view", hr);
        }
        D3D11_TEXTURE2D_DESC multi = desc;
        multi.MipLevels = 0;
        ineligible_surface_probes(device.p, multi, "Create full mipchain");
        multi.MipLevels = 2;
        ineligible_surface_probes(device.p, multi, "Create two mip texture");
        multi.MipLevels = 1;
        multi.ArraySize = 2;
        ineligible_surface_probes(device.p, multi, "Create texture array");
        D3D11_TEXTURE2D_DESC stage = desc;
        stage.Usage = D3D11_USAGE_STAGING; stage.BindFlags = 0;
        stage.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateTexture2D(&stage, nullptr, staging.out());
        bool staging_ok = result("Create staging texture", hr);
        if (staging_ok) staging_surface_probes(staging.p);
        const char *hlsl =
            "float4 vs(uint id : SV_VertexID) : SV_Position {"
            "float2 uv=float2((id<<1)&2,id&2); return float4(uv*float2(2,-2)+float2(-1,1),0,1); }"
            "float4 ps() : SV_Target { return float4(64.0/255.0,128.0/255.0,192.0/255.0,1); }";
        Ptr<ID3DBlob> vs_blob, ps_blob;
        bool vs_ok = compile(hlsl, "vs", "vs_4_0", vs_blob);
        bool ps_ok = compile(hlsl, "ps", "ps_4_0", ps_blob);
        Ptr<ID3D11VertexShader> vs;
        Ptr<ID3D11PixelShader> ps;
        if (vs_ok) { hr = device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, vs.out()); vs_ok = result("CreateVertexShader", hr); }
        if (ps_ok) { hr = device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, ps.out()); ps_ok = result("CreatePixelShader", hr); }
        bool can_draw = render_ok && staging_ok && vs_ok && ps_ok;
        if (can_draw) {
            draw(context.p, view.p, vs.p, ps.p);
            core &= readback(device.p, context.p, texture.p, staging.p, "Core staging Map");
        } else core = false;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        hr = device->CreateTexture2D(&desc, nullptr, gdi.out());
        if (result("Create GDI compatible texture", hr)) {
            hr = device->CreateRenderTargetView(gdi.p, nullptr, gdi_view.out());
            bool gdi_ok = result("Create GDI render target view", hr);
            if (gdi_ok && staging_ok && vs_ok && ps_ok) {
                draw(context.p, gdi_view.p, vs.p, ps.p);
                readback(device.p, context.p, gdi.p, staging.p, "GDI staging Map");
                gdi_roundtrip(gdi.p, staging.p, context.p, gdi_view.p, view.p);
            } else record("GDI draw and readback", E_FAIL, false);
            surface_probes(gdi.p, device.p);
        }
    }
summary:
    std::printf("\n%-40s %-5s %s\n", "CHECK", "RESULT", "HRESULT");
    for (const Check &c : checks)
        std::printf("%-40s %-5s 0x%08lx\n", c.name, c.pass ? "PASS" : "FAIL", (unsigned long)c.hr);
    for (const Check &c : checks)
        core &= c.pass;
    std::printf("Core D3D11 prerequisites: %s\n", core ? "PASS" : "FAIL");
    return core ? 0 : 1;
}
