#ifndef NPT_D3D11_SURFACE_LOCAL_H
#define NPT_D3D11_SURFACE_LOCAL_H

#include <stdbool.h>
#include "npt_com.h"

HRESULT npt_d3d11_surface_local_qi(struct npt_com_base *primary,
                                   const GUID *riid,
                                   void **out);
void npt_d3d11_surface_local_destroy(struct npt_com_base *primary);
bool npt_d3d11_surface_local_has_active_dc(struct npt_com_base *primary);

#endif
