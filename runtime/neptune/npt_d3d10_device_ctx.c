#ifdef _WIN32
#include <d3d10_1.h>
#include <d3d11_4.h>
#define NPT_D3D10_USE_SDK_HEADERS 1
#include "npt_d3d10_facets.h"
#include "npt_d3d10_device_internal.h"
#include <string.h>

static ID3D11DeviceContext *
dev_get_ctx(ID3D10Device1 *iface)
{
   ID3D11Device *dev11 = (ID3D11Device *)((struct npt_d3d10_facet_base *)iface)->owner;
   ID3D11DeviceContext *ctx = NULL;
   dev11->lpVtbl->GetImmediateContext(dev11, &ctx);
   return ctx;
}

static D3D11_BOX
box_to_11(const D3D10_BOX *box)
{
   D3D11_BOX result = {box->left, box->top, box->front, box->right, box->bottom, box->back};
   return result;
}

void STDMETHODCALLTYPE
npt_d3d10_dev_VSSetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppConstantBuffers)
{
   if (StartSlot > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || NumBuffers > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)
      return;
   ID3D11Buffer *objects11[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumBuffers; ++i)
      if (ppConstantBuffers && ppConstantBuffers[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppConstantBuffers[i], &IID_ID3D11Buffer, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->VSSetConstantBuffers(ctx, StartSlot, NumBuffers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumBuffers; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_PSSetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView *const *ppShaderResourceViews)
{
   if (StartSlot > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || NumViews > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)
      return;
   ID3D11ShaderResourceView *objects11[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumViews; ++i)
      if (ppShaderResourceViews && ppShaderResourceViews[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppShaderResourceViews[i], &IID_ID3D11ShaderResourceView, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->PSSetShaderResources(ctx, StartSlot, NumViews, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumViews; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_PSSetShader(
   ID3D10Device *This,
   ID3D10PixelShader *pPixelShader)
{
   ID3D11PixelShader *pPixelShader11 = NULL;
   if (pPixelShader) npt_d3d10_unwrap_to_d3d11((IUnknown *)pPixelShader, &IID_ID3D11PixelShader, (void **)&pPixelShader11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->PSSetShader(ctx, pPixelShader11, NULL, 0);
      ctx->lpVtbl->Release(ctx);
   }
   if (pPixelShader11) pPixelShader11->lpVtbl->Release(pPixelShader11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_PSSetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState *const *ppSamplers)
{
   if (StartSlot > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT || NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)
      return;
   ID3D11SamplerState *objects11[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumSamplers; ++i)
      if (ppSamplers && ppSamplers[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppSamplers[i], &IID_ID3D11SamplerState, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->PSSetSamplers(ctx, StartSlot, NumSamplers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumSamplers; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_VSSetShader(
   ID3D10Device *This,
   ID3D10VertexShader *pVertexShader)
{
   ID3D11VertexShader *pVertexShader11 = NULL;
   if (pVertexShader) npt_d3d10_unwrap_to_d3d11((IUnknown *)pVertexShader, &IID_ID3D11VertexShader, (void **)&pVertexShader11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->VSSetShader(ctx, pVertexShader11, NULL, 0);
      ctx->lpVtbl->Release(ctx);
   }
   if (pVertexShader11) pVertexShader11->lpVtbl->Release(pVertexShader11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_IASetInputLayout(
   ID3D10Device *This,
   ID3D10InputLayout *pInputLayout)
{
   ID3D11InputLayout *pInputLayout11 = NULL;
   if (pInputLayout) npt_d3d10_unwrap_to_d3d11((IUnknown *)pInputLayout, &IID_ID3D11InputLayout, (void **)&pInputLayout11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->IASetInputLayout(ctx, pInputLayout11);
      ctx->lpVtbl->Release(ctx);
   }
   if (pInputLayout11) pInputLayout11->lpVtbl->Release(pInputLayout11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_IASetVertexBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppVertexBuffers,
   const UINT *pStrides,
   const UINT *pOffsets)
{
   if (StartSlot > D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT || NumBuffers > D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - StartSlot)
      return;
   ID3D11Buffer *objects11[D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumBuffers; ++i)
      if (ppVertexBuffers && ppVertexBuffers[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppVertexBuffers[i], &IID_ID3D11Buffer, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->IASetVertexBuffers(ctx, StartSlot, NumBuffers, objects11, pStrides, pOffsets);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumBuffers; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_IASetIndexBuffer(
   ID3D10Device *This,
   ID3D10Buffer *pIndexBuffer,
   DXGI_FORMAT Format,
   UINT Offset)
{
   ID3D11Buffer *pIndexBuffer11 = NULL;
   if (pIndexBuffer) npt_d3d10_unwrap_to_d3d11((IUnknown *)pIndexBuffer, &IID_ID3D11Buffer, (void **)&pIndexBuffer11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->IASetIndexBuffer(ctx, pIndexBuffer11, Format, Offset);
      ctx->lpVtbl->Release(ctx);
   }
   if (pIndexBuffer11) pIndexBuffer11->lpVtbl->Release(pIndexBuffer11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_IASetPrimitiveTopology(
   ID3D10Device *This,
   D3D10_PRIMITIVE_TOPOLOGY Topology)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->IASetPrimitiveTopology(ctx, (D3D11_PRIMITIVE_TOPOLOGY)Topology);
      ctx->lpVtbl->Release(ctx);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_VSSetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView *const *ppShaderResourceViews)
{
   if (StartSlot > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || NumViews > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)
      return;
   ID3D11ShaderResourceView *objects11[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumViews; ++i)
      if (ppShaderResourceViews && ppShaderResourceViews[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppShaderResourceViews[i], &IID_ID3D11ShaderResourceView, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->VSSetShaderResources(ctx, StartSlot, NumViews, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumViews; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_VSSetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState *const *ppSamplers)
{
   if (StartSlot > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT || NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)
      return;
   ID3D11SamplerState *objects11[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumSamplers; ++i)
      if (ppSamplers && ppSamplers[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppSamplers[i], &IID_ID3D11SamplerState, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->VSSetSamplers(ctx, StartSlot, NumSamplers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumSamplers; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_GSSetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppConstantBuffers)
{
   if (StartSlot > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || NumBuffers > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)
      return;
   ID3D11Buffer *objects11[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumBuffers; ++i)
      if (ppConstantBuffers && ppConstantBuffers[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppConstantBuffers[i], &IID_ID3D11Buffer, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->GSSetConstantBuffers(ctx, StartSlot, NumBuffers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumBuffers; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_GSSetShader(
   ID3D10Device *This,
   ID3D10GeometryShader *pShader)
{
   ID3D11GeometryShader *pShader11 = NULL;
   if (pShader) npt_d3d10_unwrap_to_d3d11((IUnknown *)pShader, &IID_ID3D11GeometryShader, (void **)&pShader11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->GSSetShader(ctx, pShader11, NULL, 0);
      ctx->lpVtbl->Release(ctx);
   }
   if (pShader11) pShader11->lpVtbl->Release(pShader11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_GSSetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView *const *ppShaderResourceViews)
{
   if (StartSlot > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || NumViews > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)
      return;
   ID3D11ShaderResourceView *objects11[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumViews; ++i)
      if (ppShaderResourceViews && ppShaderResourceViews[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppShaderResourceViews[i], &IID_ID3D11ShaderResourceView, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->GSSetShaderResources(ctx, StartSlot, NumViews, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumViews; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_GSSetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState *const *ppSamplers)
{
   if (StartSlot > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT || NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)
      return;
   ID3D11SamplerState *objects11[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumSamplers; ++i)
      if (ppSamplers && ppSamplers[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppSamplers[i], &IID_ID3D11SamplerState, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->GSSetSamplers(ctx, StartSlot, NumSamplers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumSamplers; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_OMSetRenderTargets(
   ID3D10Device *This,
   UINT NumViews,
   ID3D10RenderTargetView *const *ppRenderTargetViews,
   ID3D10DepthStencilView *pDepthStencilView)
{
   if (NumViews > D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT)
      return;
   ID3D11RenderTargetView *objects11[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = {0};
   for (UINT i = 0; i < NumViews; ++i)
      if (ppRenderTargetViews && ppRenderTargetViews[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppRenderTargetViews[i], &IID_ID3D11RenderTargetView, (void **)&objects11[i]);
   ID3D11DepthStencilView *pDepthStencilView11 = NULL;
   if (pDepthStencilView) npt_d3d10_unwrap_to_d3d11((IUnknown *)pDepthStencilView, &IID_ID3D11DepthStencilView, (void **)&pDepthStencilView11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->OMSetRenderTargets(ctx, NumViews, objects11, pDepthStencilView11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumViews; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   if (pDepthStencilView11) pDepthStencilView11->lpVtbl->Release(pDepthStencilView11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_OMSetBlendState(
   ID3D10Device *This,
   ID3D10BlendState *pBlendState,
   const FLOAT BlendFactor[4],
   UINT SampleMask)
{
   ID3D11BlendState *pBlendState11 = NULL;
   if (pBlendState) npt_d3d10_unwrap_to_d3d11((IUnknown *)pBlendState, &IID_ID3D11BlendState, (void **)&pBlendState11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->OMSetBlendState(ctx, pBlendState11, BlendFactor, SampleMask);
      ctx->lpVtbl->Release(ctx);
   }
   if (pBlendState11) pBlendState11->lpVtbl->Release(pBlendState11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_OMSetDepthStencilState(
   ID3D10Device *This,
   ID3D10DepthStencilState *pDepthStencilState,
   UINT StencilRef)
{
   ID3D11DepthStencilState *pDepthStencilState11 = NULL;
   if (pDepthStencilState) npt_d3d10_unwrap_to_d3d11((IUnknown *)pDepthStencilState, &IID_ID3D11DepthStencilState, (void **)&pDepthStencilState11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->OMSetDepthStencilState(ctx, pDepthStencilState11, StencilRef);
      ctx->lpVtbl->Release(ctx);
   }
   if (pDepthStencilState11) pDepthStencilState11->lpVtbl->Release(pDepthStencilState11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_SOSetTargets(
   ID3D10Device *This,
   UINT NumBuffers,
   ID3D10Buffer *const *ppSOTargets,
   const UINT *pOffsets)
{
   if (NumBuffers > D3D10_SO_BUFFER_SLOT_COUNT)
      return;
   ID3D11Buffer *objects11[D3D10_SO_BUFFER_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumBuffers; ++i)
      if (ppSOTargets && ppSOTargets[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppSOTargets[i], &IID_ID3D11Buffer, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->SOSetTargets(ctx, NumBuffers, objects11, pOffsets);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumBuffers; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_RSSetState(
   ID3D10Device *This,
   ID3D10RasterizerState *pRasterizerState)
{
   ID3D11RasterizerState *pRasterizerState11 = NULL;
   if (pRasterizerState) npt_d3d10_unwrap_to_d3d11((IUnknown *)pRasterizerState, &IID_ID3D11RasterizerState, (void **)&pRasterizerState11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->RSSetState(ctx, pRasterizerState11);
      ctx->lpVtbl->Release(ctx);
   }
   if (pRasterizerState11) pRasterizerState11->lpVtbl->Release(pRasterizerState11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_RSSetViewports(
   ID3D10Device *This,
   UINT NumViewports,
   const D3D10_VIEWPORT *pViewports)
{
   if (NumViewports > D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) return;
   D3D11_VIEWPORT views11[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
   for (UINT i = 0; pViewports && i < NumViewports; ++i) {
      views11[i].TopLeftX = (FLOAT)pViewports[i].TopLeftX;
      views11[i].TopLeftY = (FLOAT)pViewports[i].TopLeftY;
      views11[i].Width = (FLOAT)pViewports[i].Width;
      views11[i].Height = (FLOAT)pViewports[i].Height;
      views11[i].MinDepth = pViewports[i].MinDepth;
      views11[i].MaxDepth = pViewports[i].MaxDepth;
   }
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->RSSetViewports(ctx, NumViewports, pViewports ? views11 : NULL);
      ctx->lpVtbl->Release(ctx);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_RSSetScissorRects(
   ID3D10Device *This,
   UINT NumRects,
   const D3D10_RECT *pRects)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->RSSetScissorRects(ctx, NumRects, (const D3D11_RECT *)pRects);
      ctx->lpVtbl->Release(ctx);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_SetPredication(
   ID3D10Device *This,
   ID3D10Predicate *pPredicate,
   WINBOOL PredicateValue)
{
   ID3D11Predicate *pPredicate11 = NULL;
   if (pPredicate) npt_d3d10_unwrap_to_d3d11((IUnknown *)pPredicate, &IID_ID3D11Predicate, (void **)&pPredicate11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->SetPredication(ctx, pPredicate11, PredicateValue);
      ctx->lpVtbl->Release(ctx);
   }
   if (pPredicate11) pPredicate11->lpVtbl->Release(pPredicate11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_PSSetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppConstantBuffers)
{
   if (StartSlot > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || NumBuffers > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)
      return;
   ID3D11Buffer *objects11[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {0};
   for (UINT i = 0; i < NumBuffers; ++i)
      if (ppConstantBuffers && ppConstantBuffers[i])
         npt_d3d10_unwrap_to_d3d11((IUnknown *)ppConstantBuffers[i], &IID_ID3D11Buffer, (void **)&objects11[i]);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->PSSetConstantBuffers(ctx, StartSlot, NumBuffers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumBuffers; ++i)
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_DrawIndexed(
   ID3D10Device *This,
   UINT IndexCount,
   UINT StartIndexLocation,
   INT BaseVertexLocation)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->DrawIndexed(ctx, IndexCount, StartIndexLocation, BaseVertexLocation);
      ctx->lpVtbl->Release(ctx);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_Draw(
   ID3D10Device *This,
   UINT VertexCount,
   UINT StartVertexLocation)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->Draw(ctx, VertexCount, StartVertexLocation);
      ctx->lpVtbl->Release(ctx);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_DrawIndexedInstanced(
   ID3D10Device *This,
   UINT IndexCountPerInstance,
   UINT InstanceCount,
   UINT StartIndexLocation,
   INT BaseVertexLocation,
   UINT StartInstanceLocation)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->DrawIndexedInstanced(ctx, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
      ctx->lpVtbl->Release(ctx);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_DrawInstanced(
   ID3D10Device *This,
   UINT VertexCountPerInstance,
   UINT InstanceCount,
   UINT StartVertexLocation,
   UINT StartInstanceLocation)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->DrawInstanced(ctx, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
      ctx->lpVtbl->Release(ctx);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_DrawAuto(
   ID3D10Device *This)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->DrawAuto(ctx);
      ctx->lpVtbl->Release(ctx);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_CopySubresourceRegion(
   ID3D10Device *This,
   ID3D10Resource *pDstResource,
   UINT DstSubresource,
   UINT DstX,
   UINT DstY,
   UINT DstZ,
   ID3D10Resource *pSrcResource,
   UINT SrcSubresource,
   const D3D10_BOX *pSrcBox)
{
   ID3D11Resource *pDstResource11 = NULL;
   if (pDstResource) npt_d3d10_unwrap_to_d3d11((IUnknown *)pDstResource, &IID_ID3D11Resource, (void **)&pDstResource11);
   ID3D11Resource *pSrcResource11 = NULL;
   if (pSrcResource) npt_d3d10_unwrap_to_d3d11((IUnknown *)pSrcResource, &IID_ID3D11Resource, (void **)&pSrcResource11);
   D3D11_BOX box11;
   if (pSrcBox) box11 = box_to_11(pSrcBox);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->CopySubresourceRegion(ctx, pDstResource11, DstSubresource, DstX, DstY, DstZ, pSrcResource11, SrcSubresource, pSrcBox ? &box11 : NULL);
      ctx->lpVtbl->Release(ctx);
   }
   if (pDstResource11) pDstResource11->lpVtbl->Release(pDstResource11);
   if (pSrcResource11) pSrcResource11->lpVtbl->Release(pSrcResource11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_CopyResource(
   ID3D10Device *This,
   ID3D10Resource *pDstResource,
   ID3D10Resource *pSrcResource)
{
   ID3D11Resource *pDstResource11 = NULL;
   if (pDstResource) npt_d3d10_unwrap_to_d3d11((IUnknown *)pDstResource, &IID_ID3D11Resource, (void **)&pDstResource11);
   ID3D11Resource *pSrcResource11 = NULL;
   if (pSrcResource) npt_d3d10_unwrap_to_d3d11((IUnknown *)pSrcResource, &IID_ID3D11Resource, (void **)&pSrcResource11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->CopyResource(ctx, pDstResource11, pSrcResource11);
      ctx->lpVtbl->Release(ctx);
   }
   if (pDstResource11) pDstResource11->lpVtbl->Release(pDstResource11);
   if (pSrcResource11) pSrcResource11->lpVtbl->Release(pSrcResource11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_UpdateSubresource(
   ID3D10Device *This,
   ID3D10Resource *pDstResource,
   UINT DstSubresource,
   const D3D10_BOX *pDstBox,
   const void *pSrcData,
   UINT SrcRowPitch,
   UINT SrcDepthPitch)
{
   ID3D11Resource *pDstResource11 = NULL;
   if (pDstResource) npt_d3d10_unwrap_to_d3d11((IUnknown *)pDstResource, &IID_ID3D11Resource, (void **)&pDstResource11);
   D3D11_BOX box11;
   if (pDstBox) box11 = box_to_11(pDstBox);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->UpdateSubresource(ctx, pDstResource11, DstSubresource, pDstBox ? &box11 : NULL, pSrcData, SrcRowPitch, SrcDepthPitch);
      ctx->lpVtbl->Release(ctx);
   }
   if (pDstResource11) pDstResource11->lpVtbl->Release(pDstResource11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_ClearRenderTargetView(
   ID3D10Device *This,
   ID3D10RenderTargetView *pRenderTargetView,
   const FLOAT ColorRGBA[4])
{
   ID3D11RenderTargetView *pRenderTargetView11 = NULL;
   if (pRenderTargetView) npt_d3d10_unwrap_to_d3d11((IUnknown *)pRenderTargetView, &IID_ID3D11RenderTargetView, (void **)&pRenderTargetView11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->ClearRenderTargetView(ctx, pRenderTargetView11, ColorRGBA);
      ctx->lpVtbl->Release(ctx);
   }
   if (pRenderTargetView11) pRenderTargetView11->lpVtbl->Release(pRenderTargetView11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_ClearDepthStencilView(
   ID3D10Device *This,
   ID3D10DepthStencilView *pDepthStencilView,
   UINT ClearFlags,
   FLOAT Depth,
   UINT8 Stencil)
{
   ID3D11DepthStencilView *pDepthStencilView11 = NULL;
   if (pDepthStencilView) npt_d3d10_unwrap_to_d3d11((IUnknown *)pDepthStencilView, &IID_ID3D11DepthStencilView, (void **)&pDepthStencilView11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->ClearDepthStencilView(ctx, pDepthStencilView11, ClearFlags, Depth, Stencil);
      ctx->lpVtbl->Release(ctx);
   }
   if (pDepthStencilView11) pDepthStencilView11->lpVtbl->Release(pDepthStencilView11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_GenerateMips(
   ID3D10Device *This,
   ID3D10ShaderResourceView *pShaderResourceView)
{
   ID3D11ShaderResourceView *pShaderResourceView11 = NULL;
   if (pShaderResourceView) npt_d3d10_unwrap_to_d3d11((IUnknown *)pShaderResourceView, &IID_ID3D11ShaderResourceView, (void **)&pShaderResourceView11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->GenerateMips(ctx, pShaderResourceView11);
      ctx->lpVtbl->Release(ctx);
   }
   if (pShaderResourceView11) pShaderResourceView11->lpVtbl->Release(pShaderResourceView11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_ResolveSubresource(
   ID3D10Device *This,
   ID3D10Resource *pDstResource,
   UINT DstSubresource,
   ID3D10Resource *pSrcResource,
   UINT SrcSubresource,
   DXGI_FORMAT Format)
{
   ID3D11Resource *pDstResource11 = NULL;
   if (pDstResource) npt_d3d10_unwrap_to_d3d11((IUnknown *)pDstResource, &IID_ID3D11Resource, (void **)&pDstResource11);
   ID3D11Resource *pSrcResource11 = NULL;
   if (pSrcResource) npt_d3d10_unwrap_to_d3d11((IUnknown *)pSrcResource, &IID_ID3D11Resource, (void **)&pSrcResource11);
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->ResolveSubresource(ctx, pDstResource11, DstSubresource, pSrcResource11, SrcSubresource, Format);
      ctx->lpVtbl->Release(ctx);
   }
   if (pDstResource11) pDstResource11->lpVtbl->Release(pDstResource11);
   if (pSrcResource11) pSrcResource11->lpVtbl->Release(pSrcResource11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_Flush(
   ID3D10Device *This)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->Flush(ctx);
      ctx->lpVtbl->Release(ctx);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_ClearState(
   ID3D10Device *This)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->ClearState(ctx);
      ctx->lpVtbl->Release(ctx);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_VSGetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppConstantBuffers)
{
   if (StartSlot > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || NumBuffers > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)
      return;
   ID3D11Buffer *objects11[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->VSGetConstantBuffers(ctx, StartSlot, NumBuffers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumBuffers; ++i) {
      if (ppConstantBuffers) {
         ppConstantBuffers[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10Buffer, (void **)&ppConstantBuffers[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_PSGetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView **ppShaderResourceViews)
{
   if (StartSlot > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || NumViews > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)
      return;
   ID3D11ShaderResourceView *objects11[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->PSGetShaderResources(ctx, StartSlot, NumViews, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumViews; ++i) {
      if (ppShaderResourceViews) {
         ppShaderResourceViews[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10ShaderResourceView, (void **)&ppShaderResourceViews[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_PSGetShader(
   ID3D10Device *This,
   ID3D10PixelShader **ppPixelShader)
{
   ID3D11PixelShader *ppPixelShader11 = NULL;
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->PSGetShader(ctx, &ppPixelShader11, NULL, NULL);
      ctx->lpVtbl->Release(ctx);
   }
   if (ppPixelShader) {
      *ppPixelShader = NULL;
      if (ppPixelShader11) npt_d3d10_wrap_d3d11_object((IUnknown *)ppPixelShader11, &IID_ID3D10PixelShader, (void **)ppPixelShader);
   }
   if (ppPixelShader11) ppPixelShader11->lpVtbl->Release(ppPixelShader11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_PSGetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState **ppSamplers)
{
   if (StartSlot > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT || NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)
      return;
   ID3D11SamplerState *objects11[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->PSGetSamplers(ctx, StartSlot, NumSamplers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumSamplers; ++i) {
      if (ppSamplers) {
         ppSamplers[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10SamplerState, (void **)&ppSamplers[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_VSGetShader(
   ID3D10Device *This,
   ID3D10VertexShader **ppVertexShader)
{
   ID3D11VertexShader *ppVertexShader11 = NULL;
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->VSGetShader(ctx, &ppVertexShader11, NULL, NULL);
      ctx->lpVtbl->Release(ctx);
   }
   if (ppVertexShader) {
      *ppVertexShader = NULL;
      if (ppVertexShader11) npt_d3d10_wrap_d3d11_object((IUnknown *)ppVertexShader11, &IID_ID3D10VertexShader, (void **)ppVertexShader);
   }
   if (ppVertexShader11) ppVertexShader11->lpVtbl->Release(ppVertexShader11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_PSGetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppConstantBuffers)
{
   if (StartSlot > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || NumBuffers > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)
      return;
   ID3D11Buffer *objects11[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->PSGetConstantBuffers(ctx, StartSlot, NumBuffers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumBuffers; ++i) {
      if (ppConstantBuffers) {
         ppConstantBuffers[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10Buffer, (void **)&ppConstantBuffers[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_IAGetInputLayout(
   ID3D10Device *This,
   ID3D10InputLayout **ppInputLayout)
{
   ID3D11InputLayout *ppInputLayout11 = NULL;
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->IAGetInputLayout(ctx, &ppInputLayout11);
      ctx->lpVtbl->Release(ctx);
   }
   if (ppInputLayout) {
      *ppInputLayout = NULL;
      if (ppInputLayout11) npt_d3d10_wrap_d3d11_object((IUnknown *)ppInputLayout11, &IID_ID3D10InputLayout, (void **)ppInputLayout);
   }
   if (ppInputLayout11) ppInputLayout11->lpVtbl->Release(ppInputLayout11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_IAGetVertexBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppVertexBuffers,
   UINT *pStrides,
   UINT *pOffsets)
{
   if (StartSlot > D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT || NumBuffers > D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - StartSlot)
      return;
   ID3D11Buffer *objects11[D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->IAGetVertexBuffers(ctx, StartSlot, NumBuffers, objects11, pStrides, pOffsets);
      ctx->lpVtbl->Release(ctx);
   } else {
      if (pStrides) memset(pStrides, 0, NumBuffers * sizeof(*pStrides));
      if (pOffsets) memset(pOffsets, 0, NumBuffers * sizeof(*pOffsets));
   }
   for (UINT i = 0; i < NumBuffers; ++i) {
      if (ppVertexBuffers) {
         ppVertexBuffers[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10Buffer, (void **)&ppVertexBuffers[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_IAGetIndexBuffer(
   ID3D10Device *This,
   ID3D10Buffer **pIndexBuffer,
   DXGI_FORMAT *Format,
   UINT *Offset)
{
   ID3D11Buffer *pIndexBuffer11 = NULL;
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->IAGetIndexBuffer(ctx, &pIndexBuffer11, Format, Offset);
      ctx->lpVtbl->Release(ctx);
   } else {
      if (Format) *Format = DXGI_FORMAT_UNKNOWN;
      if (Offset) *Offset = 0;
   }
   if (pIndexBuffer) {
      *pIndexBuffer = NULL;
      if (pIndexBuffer11) npt_d3d10_wrap_d3d11_object((IUnknown *)pIndexBuffer11, &IID_ID3D10Buffer, (void **)pIndexBuffer);
   }
   if (pIndexBuffer11) pIndexBuffer11->lpVtbl->Release(pIndexBuffer11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_IAGetPrimitiveTopology(
   ID3D10Device *This,
   D3D10_PRIMITIVE_TOPOLOGY *pTopology)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->IAGetPrimitiveTopology(ctx, (D3D11_PRIMITIVE_TOPOLOGY *)pTopology);
      ctx->lpVtbl->Release(ctx);
   } else if (pTopology) {
      *pTopology = D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED;
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_VSGetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView **ppShaderResourceViews)
{
   if (StartSlot > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || NumViews > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)
      return;
   ID3D11ShaderResourceView *objects11[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->VSGetShaderResources(ctx, StartSlot, NumViews, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumViews; ++i) {
      if (ppShaderResourceViews) {
         ppShaderResourceViews[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10ShaderResourceView, (void **)&ppShaderResourceViews[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_VSGetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState **ppSamplers)
{
   if (StartSlot > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT || NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)
      return;
   ID3D11SamplerState *objects11[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->VSGetSamplers(ctx, StartSlot, NumSamplers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumSamplers; ++i) {
      if (ppSamplers) {
         ppSamplers[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10SamplerState, (void **)&ppSamplers[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_GetPredication(
   ID3D10Device *This,
   ID3D10Predicate **ppPredicate,
   WINBOOL *pPredicateValue)
{
   ID3D11Predicate *ppPredicate11 = NULL;
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->GetPredication(ctx, &ppPredicate11, pPredicateValue);
      ctx->lpVtbl->Release(ctx);
   } else if (pPredicateValue) {
      *pPredicateValue = FALSE;
   }
   if (ppPredicate) {
      *ppPredicate = NULL;
      if (ppPredicate11) npt_d3d10_wrap_d3d11_object((IUnknown *)ppPredicate11, &IID_ID3D10Predicate, (void **)ppPredicate);
   }
   if (ppPredicate11) ppPredicate11->lpVtbl->Release(ppPredicate11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_GSGetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView **ppShaderResourceViews)
{
   if (StartSlot > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || NumViews > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)
      return;
   ID3D11ShaderResourceView *objects11[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->GSGetShaderResources(ctx, StartSlot, NumViews, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumViews; ++i) {
      if (ppShaderResourceViews) {
         ppShaderResourceViews[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10ShaderResourceView, (void **)&ppShaderResourceViews[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_GSGetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState **ppSamplers)
{
   if (StartSlot > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT || NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)
      return;
   ID3D11SamplerState *objects11[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->GSGetSamplers(ctx, StartSlot, NumSamplers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumSamplers; ++i) {
      if (ppSamplers) {
         ppSamplers[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10SamplerState, (void **)&ppSamplers[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_OMGetRenderTargets(
   ID3D10Device *This,
   UINT NumViews,
   ID3D10RenderTargetView **ppRenderTargetViews,
   ID3D10DepthStencilView **ppDepthStencilView)
{
   if (NumViews > D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT)
      return;
   ID3D11RenderTargetView *objects11[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = {0};
   ID3D11DepthStencilView *ppDepthStencilView11 = NULL;
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->OMGetRenderTargets(ctx, NumViews, objects11, &ppDepthStencilView11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumViews; ++i) {
      if (ppRenderTargetViews) {
         ppRenderTargetViews[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10RenderTargetView, (void **)&ppRenderTargetViews[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
   if (ppDepthStencilView) {
      *ppDepthStencilView = NULL;
      if (ppDepthStencilView11) npt_d3d10_wrap_d3d11_object((IUnknown *)ppDepthStencilView11, &IID_ID3D10DepthStencilView, (void **)ppDepthStencilView);
   }
   if (ppDepthStencilView11) ppDepthStencilView11->lpVtbl->Release(ppDepthStencilView11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_OMGetBlendState(
   ID3D10Device *This,
   ID3D10BlendState **ppBlendState,
   FLOAT BlendFactor[4],
   UINT *pSampleMask)
{
   ID3D11BlendState *ppBlendState11 = NULL;
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->OMGetBlendState(ctx, &ppBlendState11, BlendFactor, pSampleMask);
      ctx->lpVtbl->Release(ctx);
   } else {
      if (BlendFactor) memset(BlendFactor, 0, 4 * sizeof(FLOAT));
      if (pSampleMask) *pSampleMask = 0;
   }
   if (ppBlendState) {
      *ppBlendState = NULL;
      if (ppBlendState11) npt_d3d10_wrap_d3d11_object((IUnknown *)ppBlendState11, &IID_ID3D10BlendState, (void **)ppBlendState);
   }
   if (ppBlendState11) ppBlendState11->lpVtbl->Release(ppBlendState11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_OMGetDepthStencilState(
   ID3D10Device *This,
   ID3D10DepthStencilState **ppDepthStencilState,
   UINT *pStencilRef)
{
   ID3D11DepthStencilState *ppDepthStencilState11 = NULL;
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->OMGetDepthStencilState(ctx, &ppDepthStencilState11, pStencilRef);
      ctx->lpVtbl->Release(ctx);
   } else if (pStencilRef) {
      *pStencilRef = 0;
   }
   if (ppDepthStencilState) {
      *ppDepthStencilState = NULL;
      if (ppDepthStencilState11) npt_d3d10_wrap_d3d11_object((IUnknown *)ppDepthStencilState11, &IID_ID3D10DepthStencilState, (void **)ppDepthStencilState);
   }
   if (ppDepthStencilState11) ppDepthStencilState11->lpVtbl->Release(ppDepthStencilState11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_SOGetTargets(
   ID3D10Device *This,
   UINT NumBuffers,
   ID3D10Buffer **ppSOTargets,
   UINT *pOffsets)
{
   if (NumBuffers > D3D10_SO_BUFFER_SLOT_COUNT)
      return;
   ID3D11Buffer *objects11[D3D10_SO_BUFFER_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->SOGetTargets(ctx, NumBuffers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumBuffers; ++i) {
      if (ppSOTargets) {
         ppSOTargets[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10Buffer, (void **)&ppSOTargets[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
   if (pOffsets) for (UINT i = 0; i < NumBuffers; ++i) pOffsets[i] = 0;
}

void STDMETHODCALLTYPE
npt_d3d10_dev_GSGetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppConstantBuffers)
{
   if (StartSlot > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || NumBuffers > D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)
      return;
   ID3D11Buffer *objects11[D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {0};
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->GSGetConstantBuffers(ctx, StartSlot, NumBuffers, objects11);
      ctx->lpVtbl->Release(ctx);
   }
   for (UINT i = 0; i < NumBuffers; ++i) {
      if (ppConstantBuffers) {
         ppConstantBuffers[i] = NULL;
         if (objects11[i]) npt_d3d10_wrap_d3d11_object((IUnknown *)objects11[i], &IID_ID3D10Buffer, (void **)&ppConstantBuffers[i]);
      }
      if (objects11[i]) objects11[i]->lpVtbl->Release(objects11[i]);
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_GSGetShader(
   ID3D10Device *This,
   ID3D10GeometryShader **ppGeometryShader)
{
   ID3D11GeometryShader *ppGeometryShader11 = NULL;
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->GSGetShader(ctx, &ppGeometryShader11, NULL, NULL);
      ctx->lpVtbl->Release(ctx);
   }
   if (ppGeometryShader) {
      *ppGeometryShader = NULL;
      if (ppGeometryShader11) npt_d3d10_wrap_d3d11_object((IUnknown *)ppGeometryShader11, &IID_ID3D10GeometryShader, (void **)ppGeometryShader);
   }
   if (ppGeometryShader11) ppGeometryShader11->lpVtbl->Release(ppGeometryShader11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_RSGetState(
   ID3D10Device *This,
   ID3D10RasterizerState **ppRasterizerState)
{
   ID3D11RasterizerState *ppRasterizerState11 = NULL;
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->RSGetState(ctx, &ppRasterizerState11);
      ctx->lpVtbl->Release(ctx);
   }
   if (ppRasterizerState) {
      *ppRasterizerState = NULL;
      if (ppRasterizerState11) npt_d3d10_wrap_d3d11_object((IUnknown *)ppRasterizerState11, &IID_ID3D10RasterizerState, (void **)ppRasterizerState);
   }
   if (ppRasterizerState11) ppRasterizerState11->lpVtbl->Release(ppRasterizerState11);
}

void STDMETHODCALLTYPE
npt_d3d10_dev_RSGetViewports(
   ID3D10Device *This,
   UINT *NumViewports,
   D3D10_VIEWPORT *pViewports)
{
   if (!NumViewports) return;
   UINT capacity = pViewports ? *NumViewports : 0;
   if (capacity > D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) capacity = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
   UINT count11 = capacity;
   D3D11_VIEWPORT views11[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->RSGetViewports(ctx, &count11, pViewports ? views11 : NULL);
      ctx->lpVtbl->Release(ctx);
   } else {
      count11 = 0;
   }
   *NumViewports = count11;
   for (UINT i = 0; pViewports && i < capacity && i < count11; ++i) {
      pViewports[i].TopLeftX = (INT)views11[i].TopLeftX;
      pViewports[i].TopLeftY = (INT)views11[i].TopLeftY;
      pViewports[i].Width = (UINT)views11[i].Width;
      pViewports[i].Height = (UINT)views11[i].Height;
      pViewports[i].MinDepth = views11[i].MinDepth;
      pViewports[i].MaxDepth = views11[i].MaxDepth;
   }
}

void STDMETHODCALLTYPE
npt_d3d10_dev_RSGetScissorRects(
   ID3D10Device *This,
   UINT *NumRects,
   D3D10_RECT *pRects)
{
   ID3D11DeviceContext *ctx = dev_get_ctx((ID3D10Device1 *)This);
   if (ctx) {
      ctx->lpVtbl->RSGetScissorRects(ctx, NumRects, (D3D11_RECT *)pRects);
      ctx->lpVtbl->Release(ctx);
   } else if (NumRects) {
      *NumRects = 0;
   }
}

#else
typedef int npt_d3d10_device_ctx_empty_translation_unit;
#endif
