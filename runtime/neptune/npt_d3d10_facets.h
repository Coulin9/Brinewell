#ifndef NPT_D3D10_FACETS_H
#define NPT_D3D10_FACETS_H

#if defined(_WIN32)
#include <windows.h>
#include <unknwn.h>
#include <stddef.h>
#include <stdint.h>
typedef struct D3D11_BLEND_DESC D3D11_BLEND_DESC;
typedef struct D3D11_SHADER_RESOURCE_VIEW_DESC D3D11_SHADER_RESOURCE_VIEW_DESC;
#else
#include "npt_cs.h"
#endif

struct npt_com_base;

HRESULT npt_d3d10_facet_qi(struct npt_com_base *primary,
                           const GUID *riid, void **out);
void npt_d3d10_facet_destroy(struct npt_com_base *primary);
const GUID *npt_com_primary_base_iid(const struct npt_com_base *primary);
uint64_t npt_com_primary_host_id(const struct npt_com_base *primary);
void *npt_com_load_local_d3d10(struct npt_com_base *primary);
void *npt_com_publish_local_d3d10(struct npt_com_base *primary, void *candidate);
void *npt_com_take_local_d3d10(struct npt_com_base *primary);

#if defined(_WIN32) && defined(NPT_D3D10_USE_SDK_HEADERS)
struct D3D10_BLEND_DESC;
struct D3D10_BLEND_DESC1;
struct D3D10_SHADER_RESOURCE_VIEW_DESC;
struct D3D10_SHADER_RESOURCE_VIEW_DESC1;

enum npt_d3d10_facet_kind {
   NPT_D3D10_FACET_DEVICE1 = 1,
   NPT_D3D10_FACET_BUFFER,
   NPT_D3D10_FACET_TEXTURE1D,
   NPT_D3D10_FACET_TEXTURE2D,
   NPT_D3D10_FACET_TEXTURE3D,
   NPT_D3D10_FACET_SRV1,
   NPT_D3D10_FACET_RTV,
   NPT_D3D10_FACET_DSV,
   NPT_D3D10_FACET_INPUT_LAYOUT,
   NPT_D3D10_FACET_VERTEX_SHADER,
   NPT_D3D10_FACET_GEOMETRY_SHADER,
   NPT_D3D10_FACET_PIXEL_SHADER,
   NPT_D3D10_FACET_BLEND_STATE1,
   NPT_D3D10_FACET_DEPTH_STENCIL_STATE,
   NPT_D3D10_FACET_RASTERIZER_STATE,
   NPT_D3D10_FACET_SAMPLER_STATE,
   NPT_D3D10_FACET_QUERY,
   NPT_D3D10_FACET_PREDICATE,
   NPT_D3D10_FACET_COUNTER,
};

struct npt_d3d10_facet_base {
   const void *lpVtbl;
   uint64_t host_id;
   struct npt_com_base *owner;
   uint32_t kind;
};
_Static_assert(offsetof(struct npt_d3d10_facet_base, host_id) == 8,
               "npt_d3d10_facet_base: host_id must be at offset 8");

void *npt_d3d10_publish_facet(struct npt_com_base *primary,
                              struct npt_d3d10_facet_base *candidate);
HRESULT npt_d3d10_wrap_d3d11_object(IUnknown *d3d11_obj,
                                    const GUID *d3d10_riid, void **out);
HRESULT npt_d3d10_unwrap_to_d3d11(IUnknown *d3d10_obj,
                                  const GUID *d3d11_riid, void **out);
HRESULT npt_d3d10_resource_qi(struct npt_com_base *primary,
                              const GUID *riid, void **out);
HRESULT npt_d3d10_state_qi(struct npt_com_base *primary,
                           const GUID *riid, void **out);
UINT npt_d3d10_misc_flags_to_d3d11(UINT flags10);
UINT npt_d3d11_misc_flags_to_d3d10(UINT flags11);
void npt_d3d11_blend_desc_to_d3d10(const D3D11_BLEND_DESC *src, struct D3D10_BLEND_DESC *dst);
void npt_d3d11_blend_desc_to_d3d10_1(const D3D11_BLEND_DESC *src, struct D3D10_BLEND_DESC1 *dst);
void npt_d3d11_srv_desc_to_d3d10(const D3D11_SHADER_RESOURCE_VIEW_DESC *src, struct D3D10_SHADER_RESOURCE_VIEW_DESC *dst);
void npt_d3d11_srv_desc_to_d3d10_1(const D3D11_SHADER_RESOURCE_VIEW_DESC *src, struct D3D10_SHADER_RESOURCE_VIEW_DESC1 *dst);
#endif

#endif
