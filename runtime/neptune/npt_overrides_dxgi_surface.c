/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGISurface::GetDesc goes to the host, which reports the typeless
 * family format of a swapchain back buffer (npt_swapchain.c).  Patch
 * the surface desc's Format to the public typed format when the
 * surface's object is a recorded back buffer.
 *
 * A surface wrapper is another interface family's view of the back
 * buffer's object -- minted by a QI on the texture wrapper or by
 * IDXGIResource1::CreateSubresourceSurface -- so its own host id is a
 * secondary registration and the back buffer's id sits on the primary
 * wrapper, reachable through com->identity (npt_com.c).  When the host
 * answered the QI with the object's own id instead, the wrapper IS the
 * primary and self's id is the key.
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_client_idxgisurface.h"

static HRESULT NPT_STDMETHODCALLTYPE
surface_GetDesc_override(void *self, DXGI_SURFACE_DESC *pDesc)
{
   HRESULT hr = npt_idxgisurface_default_GetDesc(self, pDesc);
   if (NPT_FAILED(hr) || !pDesc)
      return hr;

   const struct npt_com_base *com = self;
   const struct npt_com_base *primary = com->identity ? com->identity : com;
   const DXGI_FORMAT pub = npt_device_backbuffer_format(
      npt_com_self_device((void *)primary), primary->base.id);
   if (pub != DXGI_FORMAT_UNKNOWN)
      pDesc->Format = pub;
   return hr;
}

void
npt_overrides_dxgi_surface_init(void)
{
   /* surface / surface1 / surface2 share one default vtbl storage, so
    * a single patch covers IDXGISurface::GetDesc on all three. */
   NPT_REGISTER_OVERRIDE(idxgisurface, GetDesc, surface_GetDesc_override);
}
