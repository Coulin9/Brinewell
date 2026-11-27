#include <windows.h>
#include <d3d10_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

template<class T> struct Ptr {
    T *p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T *operator->() const { return p; }
    T **out() { return &p; }
};
static unsigned failures;
static bool check(const char *name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
    return ok;
}
template<class T, class U> static bool identity(U *source, IUnknown *expected, const char *name) {
    Ptr<T> obj;
    Ptr<IUnknown> id;
    HRESULT hr = source->QueryInterface(__uuidof(T), reinterpret_cast<void **>(obj.out()));
    if (SUCCEEDED(hr)) hr = obj->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(id.out()));
    return check(name, SUCCEEDED(hr) && id.p == expected);
}
static bool compile(const char *src, const char *profile, Ptr<ID3DBlob> &blob) {
    Ptr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", profile,
                            0, 0, blob.out(), errors.out());
    if (errors.p) std::printf("%.*s\n", (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
    return check(profile, SUCCEEDED(hr));
}
static void draw(ID3D10Device1 *dev) {
    D3D10_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D10_USAGE_DEFAULT; desc.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
    Ptr<ID3D10Texture2D> tex, staging;
    Ptr<ID3D10RenderTargetView> rtv;
    Ptr<ID3D10ShaderResourceView1> srv;
    if (!check("CreateTexture2D", SUCCEEDED(dev->CreateTexture2D(&desc, nullptr, tex.out())))) return;
    if (!check("CreateRenderTargetView", SUCCEEDED(dev->CreateRenderTargetView(tex.p, nullptr, rtv.out())))) return;
    check("CreateShaderResourceView1", SUCCEEDED(dev->CreateShaderResourceView1(tex.p, nullptr, srv.out())));
    Ptr<ID3D10Resource> resource;
    rtv->GetResource(resource.out());
    Ptr<IUnknown> a, b;
    tex->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(a.out()));
    if (resource.p) resource->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(b.out()));
    check("RTV resource identity", a.p && a.p == b.p);
    Ptr<ID3D11Texture2D> tex11;
    HRESULT hr = tex->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(tex11.out()));
    Ptr<ID3D10Resource> wic;
    if (SUCCEEDED(hr)) hr = tex11->QueryInterface(__uuidof(ID3D10Resource), reinterpret_cast<void **>(wic.out()));
    check("D3D11 texture to D3D10 resource", SUCCEEDED(hr) && wic.p);
    desc.Usage = D3D10_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    if (!check("Create staging", SUCCEEDED(dev->CreateTexture2D(&desc, nullptr, staging.out())))) return;
    const char *vs_src = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)}; return float4(p[id],0,1); }";
    const char *ps_src = "float4 main() : SV_Target { return float4(0.250980392,0.501960784,0.752941176,1); }";
    Ptr<ID3DBlob> vs_blob, ps_blob;
    if (!compile(vs_src, "vs_4_0", vs_blob) || !compile(ps_src, "ps_4_0", ps_blob)) return;
    Ptr<ID3D10VertexShader> vs; Ptr<ID3D10PixelShader> ps;
    if (!check("CreateVertexShader", SUCCEEDED(dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), vs.out()))) ||
        !check("CreatePixelShader", SUCCEEDED(dev->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), ps.out())))) return;
    D3D10_BLEND_DESC1 bd{}; bd.RenderTarget[0].RenderTargetWriteMask = D3D10_COLOR_WRITE_ENABLE_ALL;
    Ptr<ID3D10BlendState1> blend;
    if (!check("CreateBlendState1", SUCCEEDED(dev->CreateBlendState1(&bd, blend.out())))) return;
    D3D10_RASTERIZER_DESC rd{}; rd.FillMode = D3D10_FILL_SOLID; rd.CullMode = D3D10_CULL_NONE; rd.DepthClipEnable = TRUE;
    Ptr<ID3D10RasterizerState> raster;
    if (!check("CreateRasterizerState", SUCCEEDED(dev->CreateRasterizerState(&rd, raster.out())))) return;
    ID3D10RenderTargetView *target = rtv.p; dev->OMSetRenderTargets(1, &target, nullptr);
    float factor[4]{}; dev->OMSetBlendState(blend.p, factor, ~0u); dev->RSSetState(raster.p);
    D3D10_VIEWPORT vp{0, 0, 64, 64, 0, 1}; dev->RSSetViewports(1, &vp);
    dev->VSSetShader(vs.p); dev->PSSetShader(ps.p); dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev->Draw(3, 0); dev->OMSetRenderTargets(0, nullptr, nullptr); dev->CopyResource(staging.p, tex.p);
    D3D10_MAPPED_TEXTURE2D mapped{};
    if (!check("Map staging", SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &mapped)))) return;
    unsigned bad = 0;
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
        const auto *p = static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch + x * 4;
        if (p[0] != 0xc0 || p[1] != 0x80 || p[2] != 0x40 || p[3] != 0xff) ++bad;
    }
    staging->Unmap(0); std::printf("bad pixels: %u\n", bad); check("readback", bad == 0);

    /* Regression: Medium #5 — GetDesc(NULL) on state objects must not crash. */
    blend->GetDesc(nullptr);
    blend->GetDesc1(nullptr);
    raster->GetDesc(nullptr);
    rtv->GetDesc(nullptr);
    srv->GetDesc(nullptr);
    srv->GetDesc1(nullptr);
    check("state GetDesc(NULL) safe", true);

    /* Regression: Critical #2 — host wire calls encoding local facets via
     * npt_object_get_id() (offset 8 host_id) must translate to the real primary
     * host object without a fatal host handle-miss error:
     *  (a) IDXGIDevice::QueryResourceResidency on {ID3D10Texture2D*, IDXGISurface*}
     *      must match calling it on the primary ID3D11Texture2D* (S_OK or E_NOTIMPL
     *      on D3DMetal) and keep the ring/device healthy.
     *  (b) ID3D11Device::SetPrivateDataInterface with ID3D10Texture2D* and local
     *      IDXGISurface* must succeed cleanly with S_OK. */
    Ptr<IDXGIDevice> dxgi_dev;
    Ptr<ID3D11Device> dev11;
    Ptr<IDXGISurface> surface;
    hr = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(dxgi_dev.out()));
    if (SUCCEEDED(hr))
        hr = dev->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void **>(dev11.out()));
    if (SUCCEEDED(hr))
        hr = tex->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void **>(surface.out()));
    if (check("QI IDXGIDevice + ID3D11Device + IDXGISurface", SUCCEEDED(hr) && dxgi_dev.p && dev11.p && surface.p)) {
        IUnknown *primary_res[1] = { tex11.p };
        DXGI_RESIDENCY primary_status[1]{};
        HRESULT hr_primary = dxgi_dev->QueryResourceResidency(primary_res, primary_status, 1);
        IUnknown *resources[2] = { tex.p, surface.p };
        DXGI_RESIDENCY residency[2]{};
        hr = dxgi_dev->QueryResourceResidency(resources, residency, 2);
        std::printf("QueryResourceResidency hr=0x%08lx (primary=0x%08lx)\n",
                    static_cast<unsigned long>(hr), static_cast<unsigned long>(hr_primary));
        check("QueryResourceResidency D3D10Tex + local Surface",
              hr == hr_primary && (SUCCEEDED(hr) || hr == E_NOTIMPL) &&
              dev->GetDeviceRemovedReason() == S_OK);
        static const GUID kTagTex = { 0x6f7c2a11, 0x4b5e, 0x4c9a, { 0x81, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x01 } };
        static const GUID kTagSurf = { 0x6f7c2a11, 0x4b5e, 0x4c9a, { 0x81, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x02 } };
        HRESULT hr_pdi_tex = dev11->SetPrivateDataInterface(kTagTex, tex.p);
        HRESULT hr_pdi_surf = dev11->SetPrivateDataInterface(kTagSurf, surface.p);
        dev11->SetPrivateDataInterface(kTagTex, nullptr);
        dev11->SetPrivateDataInterface(kTagSurf, nullptr);
        check("SetPrivateDataInterface D3D10Tex + local Surface",
              SUCCEEDED(hr_pdi_tex) && SUCCEEDED(hr_pdi_surf));
    }

    /* Regression: Medium #6 — CreateTexture2D with invalid MipLevels=99 (and
     * ArraySize=0) + non-NULL pInitialData must return E_INVALIDARG cleanly. */
    uint32_t dummy_pixel = 0xff00ff00u;
    D3D10_SUBRESOURCE_DATA init_data{};
    init_data.pSysMem = &dummy_pixel;
    init_data.SysMemPitch = 4;
    D3D10_TEXTURE2D_DESC bad_desc{};
    bad_desc.Width = 64; bad_desc.Height = 64; bad_desc.MipLevels = 99; bad_desc.ArraySize = 1;
    bad_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bad_desc.SampleDesc.Count = 1;
    bad_desc.Usage = D3D10_USAGE_DEFAULT; bad_desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    Ptr<ID3D10Texture2D> bad_tex;
    hr = dev->CreateTexture2D(&bad_desc, &init_data, bad_tex.out());
    check("CreateTexture2D MipLevels=99 E_INVALIDARG", hr == E_INVALIDARG && !bad_tex.p);
    bad_desc.MipLevels = 1; bad_desc.ArraySize = 0;
    hr = dev->CreateTexture2D(&bad_desc, &init_data, bad_tex.out());
    check("CreateTexture2D ArraySize=0 E_INVALIDARG", hr == E_INVALIDARG && !bad_tex.p);
}

static void swapchain_from_d3d10(IDXGIFactory *factory, ID3D10Device *dev) {
    /* Regression: Critical #1 — CreateSwapChain passed an ID3D10Device* facet
     * must resolve ID3D11Device before reading npt_device and succeed cleanly. */
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "npt_d3d10_sc_test";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "d3d10_sc", WS_OVERLAPPEDWINDOW,
                                0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!check("swapchain hwnd", hwnd != nullptr)) return;
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferDesc.Width = 64;
    scd.BufferDesc.Height = 64;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.OutputWindow = hwnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    Ptr<IDXGISwapChain> sc;
    HRESULT hr = factory->CreateSwapChain(dev, &scd, sc.out());
    check("CreateSwapChain(ID3D10Device*)", SUCCEEDED(hr) && sc.p != nullptr);
    if (SUCCEEDED(hr) && sc.p) {
        Ptr<ID3D10Texture2D> bb;
        HRESULT bhr = sc->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void **>(bb.out()));
        check("SwapChain GetBuffer(ID3D10Texture2D)", SUCCEEDED(bhr) && bb.p != nullptr);
    }
    if (sc.p) { sc.p->Release(); sc.p = nullptr; }
    DestroyWindow(hwnd);
}
int main() {
    HMODULE core = LoadLibraryA("d3d10core.dll"), d10 = LoadLibraryA("d3d10.dll"), d101 = LoadLibraryA("d3d10_1.dll");
    if (!check("load DLLs", core && d10 && d101)) return 1;
    using Version = UINT64 (WINAPI *)(); using Register = HRESULT (WINAPI *)(const void *, UINT);
    using CoreCreate = HRESULT (WINAPI *)(IDXGIFactory *, IDXGIAdapter *, UINT, D3D_FEATURE_LEVEL, ID3D10Device **);
    using Create = HRESULT (WINAPI *)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device **);
    using Create1 = HRESULT (WINAPI *)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1, UINT, ID3D10Device1 **);
    auto version = reinterpret_cast<Version>(GetProcAddress(core, "D3D10CoreGetVersion"));
    auto reg = reinterpret_cast<Register>(GetProcAddress(core, "D3D10CoreRegisterLayers"));
    auto create_core = reinterpret_cast<CoreCreate>(GetProcAddress(core, "D3D10CoreCreateDevice"));
    auto create = reinterpret_cast<Create>(GetProcAddress(d10, "D3D10CreateDevice"));
    auto create1 = reinterpret_cast<Create1>(GetProcAddress(d101, "D3D10CreateDevice1"));
    if (!check("exports", version && reg && create_core && create && create1)) return 1;
    check("version", version() == 0x000a000100041770ULL); check("register layers", reg(nullptr, 0) == E_NOTIMPL);
    Ptr<IDXGIFactory> factory; Ptr<IDXGIAdapter> adapter;
    if (!check("factory", SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void **>(factory.out())))) ||
        !check("adapter", SUCCEEDED(factory->EnumAdapters(0, adapter.out())))) return 1;
    check("core null out", create_core(factory.p, adapter.p, 0, D3D_FEATURE_LEVEL_10_0, nullptr) == E_INVALIDARG);
    ID3D10Device *raw = nullptr;
    check("core null adapter", create_core(factory.p, nullptr, 0, D3D_FEATURE_LEVEL_10_0, &raw) == E_INVALIDARG && !raw);
    check("core invalid zero", create_core(factory.p, adapter.p, 0, static_cast<D3D_FEATURE_LEVEL>(0), &raw) == E_INVALIDARG && !raw);
    check("core invalid 11", create_core(factory.p, adapter.p, 0, static_cast<D3D_FEATURE_LEVEL>(0xb000), &raw) == E_INVALIDARG && !raw);
    for (int i = 0; i < 2; ++i) { Ptr<ID3D10Device> dev; check(i ? "core null factory" : "core factory", SUCCEEDED(create_core(i ? nullptr : factory.p, adapter.p, D3D10_CREATE_DEVICE_BGRA_SUPPORT, D3D_FEATURE_LEVEL_10_0, dev.out())) && dev.p); }
    Ptr<ID3D10Device> old;
    check("D3D10CreateDevice", SUCCEEDED(create(adapter.p, D3D10_DRIVER_TYPE_HARDWARE, nullptr, D3D10_CREATE_DEVICE_BGRA_SUPPORT, D3D10_SDK_VERSION, old.out())) && old.p);
    for (int single = 0; single < 2; ++single) {
        Ptr<ID3D10Device1> dev;
        UINT flags = D3D10_CREATE_DEVICE_BGRA_SUPPORT | (single ? D3D10_CREATE_DEVICE_SINGLETHREADED : 0);
        if (!check("D3D10CreateDevice1", SUCCEEDED(create1(adapter.p, D3D10_DRIVER_TYPE_HARDWARE, nullptr, flags, D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, dev.out())) && dev.p)) continue;
        check("feature level", dev->GetFeatureLevel() == D3D10_FEATURE_LEVEL_10_0);
        Ptr<IUnknown> id; dev->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(id.out()));
        identity<ID3D10Device>(dev.p, id.p, "identity device"); identity<ID3D10Device1>(dev.p, id.p, "identity device1");
        identity<ID3D10Multithread>(dev.p, id.p, "identity multithread"); identity<IDXGIDevice>(dev.p, id.p, "identity DXGI");
        identity<ID3D11Device>(dev.p, id.p, "identity D3D11"); identity<ID3D11Device1>(dev.p, id.p, "identity D3D11.1");
        Ptr<ID3D10Multithread> mt;
        HRESULT hr = dev->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void **>(mt.out()));
        check("multithread default", SUCCEEDED(hr) && !!mt->GetMultithreadProtected() == !single);
        if (!single) {
            draw(dev.p);
            swapchain_from_d3d10(factory.p, dev.p);
        }
    }
    return failures ? 1 : 0;
}
