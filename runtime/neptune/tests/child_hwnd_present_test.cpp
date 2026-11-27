#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>

template<typename T> struct Ptr {
    T *p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    T **out() { return &p; }
};

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void pump_messages(void) {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int main(int argc, char **argv) {
    DWORD hold_ms = 500;
    if (argc > 1) {
        long v = std::strtol(argv[1], nullptr, 10);
        if (v >= 0 && v <= 60000)
            hold_ms = static_cast<DWORD>(v);
    }

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    HBRUSH blue_brush = CreateSolidBrush(RGB(0, 0, 128));
    if (!blue_brush) {
        std::fprintf(stderr, "FAIL: CreateSolidBrush\n");
        return 1;
    }

    WNDCLASSW wc_parent{};
    wc_parent.lpfnWndProc = wnd_proc;
    wc_parent.hInstance = hinst;
    wc_parent.hbrBackground = blue_brush;
    wc_parent.lpszClassName = L"NptChildHwndParentClass";
    if (!RegisterClassW(&wc_parent)) {
        std::fprintf(stderr, "FAIL: RegisterClassW(parent) err=%lu\n", GetLastError());
        DeleteObject(blue_brush);
        return 1;
    }

    WNDCLASSW wc_child{};
    wc_child.lpfnWndProc = wnd_proc;
    wc_child.hInstance = hinst;
    wc_child.hbrBackground = nullptr;
    wc_child.lpszClassName = L"NptChildHwndChildClass";
    if (!RegisterClassW(&wc_child)) {
        std::fprintf(stderr, "FAIL: RegisterClassW(child) err=%lu\n", GetLastError());
        DeleteObject(blue_brush);
        return 1;
    }

    HWND parent = CreateWindowExW(
        0, wc_parent.lpszClassName, L"child_hwnd_present_test",
        WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
        100, 100, 640, 480,
        nullptr, nullptr, hinst, nullptr);
    if (!parent) {
        std::fprintf(stderr, "FAIL: CreateWindowExW(parent) err=%lu\n", GetLastError());
        DeleteObject(blue_brush);
        return 1;
    }

    HWND child = CreateWindowExW(
        0, wc_child.lpszClassName, L"child_viewport",
        WS_CHILD | WS_VISIBLE,
        64, 48, 320, 240,
        parent, nullptr, hinst, nullptr);
    if (!child) {
        std::fprintf(stderr, "FAIL: CreateWindowExW(child) err=%lu\n", GetLastError());
        DestroyWindow(parent);
        DeleteObject(blue_brush);
        return 1;
    }

    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);
    ShowWindow(child, SW_SHOW);
    UpdateWindow(child);
    pump_messages();

    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        device.out(), &fl, ctx.out());
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL: D3D11CreateDevice hr=0x%08lx\n", (unsigned long)hr);
        DestroyWindow(parent);
        DeleteObject(blue_brush);
        return 1;
    }

    Ptr<IDXGIDevice> dxgi_dev;
    Ptr<IDXGIAdapter> adapter;
    Ptr<IDXGIFactory2> factory2;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), (void **)dxgi_dev.out());
    if (SUCCEEDED(hr))
        hr = dxgi_dev->GetAdapter(adapter.out());
    if (SUCCEEDED(hr))
        hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void **)factory2.out());
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL: IDXGIFactory2 lookup hr=0x%08lx\n", (unsigned long)hr);
        DestroyWindow(parent);
        DeleteObject(blue_brush);
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = 320;
    sc_desc.Height = 240;
    sc_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 1;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    Ptr<IDXGISwapChain1> swapchain;
    hr = factory2->CreateSwapChainForHwnd(
        device.p, child, &sc_desc, nullptr, nullptr, swapchain.out());
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL: CreateSwapChainForHwnd(child) hr=0x%08lx\n", (unsigned long)hr);
        DestroyWindow(parent);
        DeleteObject(blue_brush);
        return 1;
    }

    Ptr<ID3D11Texture2D> backbuffer;
    hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)backbuffer.out());
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL: GetBuffer hr=0x%08lx\n", (unsigned long)hr);
        DestroyWindow(parent);
        DeleteObject(blue_brush);
        return 1;
    }

    Ptr<ID3D11RenderTargetView> rtv;
    hr = device->CreateRenderTargetView(backbuffer.p, nullptr, rtv.out());
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL: CreateRenderTargetView hr=0x%08lx\n", (unsigned long)hr);
        DestroyWindow(parent);
        DeleteObject(blue_brush);
        return 1;
    }

    const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    for (int i = 0; i < 6; ++i) {
        ctx->ClearRenderTargetView(rtv.p, green);
        hr = swapchain->Present(0, 0);
        std::printf("Present[%d] hr=0x%08lx\n", i, (unsigned long)hr);
        std::fflush(stdout);
        if (FAILED(hr) || hr == E_FAIL || hr == DXGI_STATUS_OCCLUDED) {
            std::fprintf(stderr, "FAIL: Present[%d] returned 0x%08lx\n", i, (unsigned long)hr);
            DestroyWindow(parent);
            DeleteObject(blue_brush);
            return 1;
        }
        pump_messages();
        Sleep(16);
    }

    std::printf("CHILD_HWND_PRESENT_READY parent=%p child=%p\n", (void *)parent, (void *)child);
    std::fflush(stdout);

    DWORD start = GetTickCount();
    while (GetTickCount() - start < hold_ms) {
        pump_messages();
        Sleep(20);
    }

    DestroyWindow(parent);
    DeleteObject(blue_brush);
    std::printf("CHILD_HWND_PRESENT_TEST: PASS\n");
    std::fflush(stdout);
    return 0;
}
