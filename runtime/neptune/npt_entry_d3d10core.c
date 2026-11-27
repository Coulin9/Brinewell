#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d10_1.h>
#include <d3d11_4.h>

UINT64 WINAPI D3D10CoreGetVersion(void);
HRESULT WINAPI D3D10CoreRegisterLayers(const void *layers, UINT num_layers);
HRESULT WINAPI D3D10CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter,
                                     UINT flags, D3D_FEATURE_LEVEL feature_level,
                                     ID3D10Device **device);

UINT64 WINAPI
D3D10CoreGetVersion(void)
{
   return 0x000a000100041770ULL;
}

HRESULT WINAPI
D3D10CoreRegisterLayers(const void *layers, UINT num_layers)
{
   (void)layers;
   (void)num_layers;
   return E_NOTIMPL;
}

HRESULT WINAPI
D3D10CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter, UINT flags,
                      D3D_FEATURE_LEVEL feature_level, ID3D10Device **device)
{
   (void)factory;
   if (!device)
      return E_INVALIDARG;
   *device = NULL;
   if (!adapter)
      return E_INVALIDARG;

   switch (feature_level) {
   case D3D_FEATURE_LEVEL_9_1:
   case D3D_FEATURE_LEVEL_9_2:
   case D3D_FEATURE_LEVEL_9_3:
   case D3D_FEATURE_LEVEL_10_0:
   case D3D_FEATURE_LEVEL_10_1:
      break;
   default:
      return E_INVALIDARG;
   }

   UINT flags11 = 0;
   if (flags & D3D10_CREATE_DEVICE_SINGLETHREADED)
      flags11 |= D3D11_CREATE_DEVICE_SINGLETHREADED;
   if (flags & D3D10_CREATE_DEVICE_DEBUG)
      flags11 |= D3D11_CREATE_DEVICE_DEBUG;
   if (flags & D3D10_CREATE_DEVICE_SWITCH_TO_REF)
      flags11 |= D3D11_CREATE_DEVICE_SWITCH_TO_REF;
   if (flags & D3D10_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS)
      flags11 |= D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
   if (flags & D3D10_CREATE_DEVICE_BGRA_SUPPORT)
      flags11 |= D3D11_CREATE_DEVICE_BGRA_SUPPORT;
   if (flags & D3D10_CREATE_DEVICE_DEBUGGABLE)
      flags11 |= D3D11_CREATE_DEVICE_DEBUGGABLE;
   if (flags & D3D10_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY)
      flags11 |= D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY;

   ID3D11Device *device11 = NULL;
   D3D_FEATURE_LEVEL actual = 0;
   HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                  flags11, &feature_level, 1,
                                  D3D11_SDK_VERSION, &device11, &actual, NULL);
   if (FAILED(hr) || !device11) {
      if (device11)
         ID3D11Device_Release(device11);
      return FAILED(hr) ? hr : E_FAIL;
   }
   if (actual != feature_level) {
      ID3D11Device_Release(device11);
      return DXGI_ERROR_UNSUPPORTED;
   }

   ID3D11Multithread *multithread = NULL;
   hr = ID3D11Device_QueryInterface(device11, &IID_ID3D11Multithread,
                                     (void **)&multithread);
   if (SUCCEEDED(hr) && multithread) {
      ID3D11Multithread_SetMultithreadProtected(
         multithread, !(flags & D3D10_CREATE_DEVICE_SINGLETHREADED));
   }
   if (multithread)
      ID3D11Multithread_Release(multithread);

   ID3D10Device1 *device10 = NULL;
   hr = ID3D11Device_QueryInterface(device11, &IID_ID3D10Device1,
                                     (void **)&device10);
   ID3D11Device_Release(device11);
   if (FAILED(hr) || !device10) {
      if (device10)
         ID3D10Device1_Release(device10);
      return FAILED(hr) ? hr : E_FAIL;
   }
   *device = (ID3D10Device *)device10;
   return S_OK;
}
