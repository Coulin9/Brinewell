#ifndef NPT_D3D10_DEVICE_INTERNAL_H
#define NPT_D3D10_DEVICE_INTERNAL_H

#ifdef _WIN32
#include <d3d10_1.h>

void STDMETHODCALLTYPE npt_d3d10_dev_VSSetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppConstantBuffers);
void STDMETHODCALLTYPE npt_d3d10_dev_PSSetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView *const *ppShaderResourceViews);
void STDMETHODCALLTYPE npt_d3d10_dev_PSSetShader(
   ID3D10Device *This,
   ID3D10PixelShader *pPixelShader);
void STDMETHODCALLTYPE npt_d3d10_dev_PSSetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState *const *ppSamplers);
void STDMETHODCALLTYPE npt_d3d10_dev_VSSetShader(
   ID3D10Device *This,
   ID3D10VertexShader *pVertexShader);
void STDMETHODCALLTYPE npt_d3d10_dev_IASetInputLayout(
   ID3D10Device *This,
   ID3D10InputLayout *pInputLayout);
void STDMETHODCALLTYPE npt_d3d10_dev_IASetVertexBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppVertexBuffers,
   const UINT *pStrides,
   const UINT *pOffsets);
void STDMETHODCALLTYPE npt_d3d10_dev_IASetIndexBuffer(
   ID3D10Device *This,
   ID3D10Buffer *pIndexBuffer,
   DXGI_FORMAT Format,
   UINT Offset);
void STDMETHODCALLTYPE npt_d3d10_dev_IASetPrimitiveTopology(
   ID3D10Device *This,
   D3D10_PRIMITIVE_TOPOLOGY Topology);
void STDMETHODCALLTYPE npt_d3d10_dev_VSSetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView *const *ppShaderResourceViews);
void STDMETHODCALLTYPE npt_d3d10_dev_VSSetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState *const *ppSamplers);
void STDMETHODCALLTYPE npt_d3d10_dev_GSSetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppConstantBuffers);
void STDMETHODCALLTYPE npt_d3d10_dev_GSSetShader(
   ID3D10Device *This,
   ID3D10GeometryShader *pShader);
void STDMETHODCALLTYPE npt_d3d10_dev_GSSetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView *const *ppShaderResourceViews);
void STDMETHODCALLTYPE npt_d3d10_dev_GSSetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState *const *ppSamplers);
void STDMETHODCALLTYPE npt_d3d10_dev_OMSetRenderTargets(
   ID3D10Device *This,
   UINT NumViews,
   ID3D10RenderTargetView *const *ppRenderTargetViews,
   ID3D10DepthStencilView *pDepthStencilView);
void STDMETHODCALLTYPE npt_d3d10_dev_OMSetBlendState(
   ID3D10Device *This,
   ID3D10BlendState *pBlendState,
   const FLOAT BlendFactor[4],
   UINT SampleMask);
void STDMETHODCALLTYPE npt_d3d10_dev_OMSetDepthStencilState(
   ID3D10Device *This,
   ID3D10DepthStencilState *pDepthStencilState,
   UINT StencilRef);
void STDMETHODCALLTYPE npt_d3d10_dev_SOSetTargets(
   ID3D10Device *This,
   UINT NumBuffers,
   ID3D10Buffer *const *ppSOTargets,
   const UINT *pOffsets);
void STDMETHODCALLTYPE npt_d3d10_dev_RSSetState(
   ID3D10Device *This,
   ID3D10RasterizerState *pRasterizerState);
void STDMETHODCALLTYPE npt_d3d10_dev_RSSetViewports(
   ID3D10Device *This,
   UINT NumViewports,
   const D3D10_VIEWPORT *pViewports);
void STDMETHODCALLTYPE npt_d3d10_dev_RSSetScissorRects(
   ID3D10Device *This,
   UINT NumRects,
   const D3D10_RECT *pRects);
void STDMETHODCALLTYPE npt_d3d10_dev_SetPredication(
   ID3D10Device *This,
   ID3D10Predicate *pPredicate,
   WINBOOL PredicateValue);
void STDMETHODCALLTYPE npt_d3d10_dev_PSSetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer *const *ppConstantBuffers);
void STDMETHODCALLTYPE npt_d3d10_dev_DrawIndexed(
   ID3D10Device *This,
   UINT IndexCount,
   UINT StartIndexLocation,
   INT BaseVertexLocation);
void STDMETHODCALLTYPE npt_d3d10_dev_Draw(
   ID3D10Device *This,
   UINT VertexCount,
   UINT StartVertexLocation);
void STDMETHODCALLTYPE npt_d3d10_dev_DrawIndexedInstanced(
   ID3D10Device *This,
   UINT IndexCountPerInstance,
   UINT InstanceCount,
   UINT StartIndexLocation,
   INT BaseVertexLocation,
   UINT StartInstanceLocation);
void STDMETHODCALLTYPE npt_d3d10_dev_DrawInstanced(
   ID3D10Device *This,
   UINT VertexCountPerInstance,
   UINT InstanceCount,
   UINT StartVertexLocation,
   UINT StartInstanceLocation);
void STDMETHODCALLTYPE npt_d3d10_dev_DrawAuto(
   ID3D10Device *This);
void STDMETHODCALLTYPE npt_d3d10_dev_CopySubresourceRegion(
   ID3D10Device *This,
   ID3D10Resource *pDstResource,
   UINT DstSubresource,
   UINT DstX,
   UINT DstY,
   UINT DstZ,
   ID3D10Resource *pSrcResource,
   UINT SrcSubresource,
   const D3D10_BOX *pSrcBox);
void STDMETHODCALLTYPE npt_d3d10_dev_CopyResource(
   ID3D10Device *This,
   ID3D10Resource *pDstResource,
   ID3D10Resource *pSrcResource);
void STDMETHODCALLTYPE npt_d3d10_dev_UpdateSubresource(
   ID3D10Device *This,
   ID3D10Resource *pDstResource,
   UINT DstSubresource,
   const D3D10_BOX *pDstBox,
   const void *pSrcData,
   UINT SrcRowPitch,
   UINT SrcDepthPitch);
void STDMETHODCALLTYPE npt_d3d10_dev_ClearRenderTargetView(
   ID3D10Device *This,
   ID3D10RenderTargetView *pRenderTargetView,
   const FLOAT ColorRGBA[4]);
void STDMETHODCALLTYPE npt_d3d10_dev_ClearDepthStencilView(
   ID3D10Device *This,
   ID3D10DepthStencilView *pDepthStencilView,
   UINT ClearFlags,
   FLOAT Depth,
   UINT8 Stencil);
void STDMETHODCALLTYPE npt_d3d10_dev_GenerateMips(
   ID3D10Device *This,
   ID3D10ShaderResourceView *pShaderResourceView);
void STDMETHODCALLTYPE npt_d3d10_dev_ResolveSubresource(
   ID3D10Device *This,
   ID3D10Resource *pDstResource,
   UINT DstSubresource,
   ID3D10Resource *pSrcResource,
   UINT SrcSubresource,
   DXGI_FORMAT Format);
void STDMETHODCALLTYPE npt_d3d10_dev_Flush(
   ID3D10Device *This);
void STDMETHODCALLTYPE npt_d3d10_dev_ClearState(
   ID3D10Device *This);
void STDMETHODCALLTYPE npt_d3d10_dev_VSGetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppConstantBuffers);
void STDMETHODCALLTYPE npt_d3d10_dev_PSGetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView **ppShaderResourceViews);
void STDMETHODCALLTYPE npt_d3d10_dev_PSGetShader(
   ID3D10Device *This,
   ID3D10PixelShader **ppPixelShader);
void STDMETHODCALLTYPE npt_d3d10_dev_PSGetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState **ppSamplers);
void STDMETHODCALLTYPE npt_d3d10_dev_VSGetShader(
   ID3D10Device *This,
   ID3D10VertexShader **ppVertexShader);
void STDMETHODCALLTYPE npt_d3d10_dev_PSGetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppConstantBuffers);
void STDMETHODCALLTYPE npt_d3d10_dev_IAGetInputLayout(
   ID3D10Device *This,
   ID3D10InputLayout **ppInputLayout);
void STDMETHODCALLTYPE npt_d3d10_dev_IAGetVertexBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppVertexBuffers,
   UINT *pStrides,
   UINT *pOffsets);
void STDMETHODCALLTYPE npt_d3d10_dev_IAGetIndexBuffer(
   ID3D10Device *This,
   ID3D10Buffer **pIndexBuffer,
   DXGI_FORMAT *Format,
   UINT *Offset);
void STDMETHODCALLTYPE npt_d3d10_dev_IAGetPrimitiveTopology(
   ID3D10Device *This,
   D3D10_PRIMITIVE_TOPOLOGY *pTopology);
void STDMETHODCALLTYPE npt_d3d10_dev_VSGetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView **ppShaderResourceViews);
void STDMETHODCALLTYPE npt_d3d10_dev_VSGetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState **ppSamplers);
void STDMETHODCALLTYPE npt_d3d10_dev_GetPredication(
   ID3D10Device *This,
   ID3D10Predicate **ppPredicate,
   WINBOOL *pPredicateValue);
void STDMETHODCALLTYPE npt_d3d10_dev_GSGetShaderResources(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumViews,
   ID3D10ShaderResourceView **ppShaderResourceViews);
void STDMETHODCALLTYPE npt_d3d10_dev_GSGetSamplers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumSamplers,
   ID3D10SamplerState **ppSamplers);
void STDMETHODCALLTYPE npt_d3d10_dev_OMGetRenderTargets(
   ID3D10Device *This,
   UINT NumViews,
   ID3D10RenderTargetView **ppRenderTargetViews,
   ID3D10DepthStencilView **ppDepthStencilView);
void STDMETHODCALLTYPE npt_d3d10_dev_OMGetBlendState(
   ID3D10Device *This,
   ID3D10BlendState **ppBlendState,
   FLOAT BlendFactor[4],
   UINT *pSampleMask);
void STDMETHODCALLTYPE npt_d3d10_dev_OMGetDepthStencilState(
   ID3D10Device *This,
   ID3D10DepthStencilState **ppDepthStencilState,
   UINT *pStencilRef);
void STDMETHODCALLTYPE npt_d3d10_dev_SOGetTargets(
   ID3D10Device *This,
   UINT NumBuffers,
   ID3D10Buffer **ppSOTargets,
   UINT *pOffsets);
void STDMETHODCALLTYPE npt_d3d10_dev_GSGetConstantBuffers(
   ID3D10Device *This,
   UINT StartSlot,
   UINT NumBuffers,
   ID3D10Buffer **ppConstantBuffers);
void STDMETHODCALLTYPE npt_d3d10_dev_GSGetShader(
   ID3D10Device *This,
   ID3D10GeometryShader **ppGeometryShader);
void STDMETHODCALLTYPE npt_d3d10_dev_RSGetState(
   ID3D10Device *This,
   ID3D10RasterizerState **ppRasterizerState);
void STDMETHODCALLTYPE npt_d3d10_dev_RSGetViewports(
   ID3D10Device *This,
   UINT *NumViewports,
   D3D10_VIEWPORT *pViewports);
void STDMETHODCALLTYPE npt_d3d10_dev_RSGetScissorRects(
   ID3D10Device *This,
   UINT *NumRects,
   D3D10_RECT *pRects);


HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateBuffer(
   ID3D10Device *This,
   const D3D10_BUFFER_DESC *pDesc,
   const D3D10_SUBRESOURCE_DATA *pInitialData,
   ID3D10Buffer **ppBuffer);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateTexture1D(
   ID3D10Device *This,
   const D3D10_TEXTURE1D_DESC *pDesc,
   const D3D10_SUBRESOURCE_DATA *pInitialData,
   ID3D10Texture1D **ppTexture1D);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateTexture2D(
   ID3D10Device *This,
   const D3D10_TEXTURE2D_DESC *pDesc,
   const D3D10_SUBRESOURCE_DATA *pInitialData,
   ID3D10Texture2D **ppTexture2D);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateTexture3D(
   ID3D10Device *This,
   const D3D10_TEXTURE3D_DESC *pDesc,
   const D3D10_SUBRESOURCE_DATA *pInitialData,
   ID3D10Texture3D **ppTexture3D);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateShaderResourceView(
   ID3D10Device *This,
   ID3D10Resource *pResource,
   const D3D10_SHADER_RESOURCE_VIEW_DESC *pDesc,
   ID3D10ShaderResourceView **ppSRView);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateRenderTargetView(
   ID3D10Device *This,
   ID3D10Resource *pResource,
   const D3D10_RENDER_TARGET_VIEW_DESC *pDesc,
   ID3D10RenderTargetView **ppRTView);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateDepthStencilView(
   ID3D10Device *This,
   ID3D10Resource *pResource,
   const D3D10_DEPTH_STENCIL_VIEW_DESC *pDesc,
   ID3D10DepthStencilView **ppDepthStencilView);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateInputLayout(
   ID3D10Device *This,
   const D3D10_INPUT_ELEMENT_DESC *pInputElementDescs,
   UINT NumElements,
   const void *pShaderBytecodeWithInputSignature,
   SIZE_T BytecodeLength,
   ID3D10InputLayout **ppInputLayout);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateVertexShader(
   ID3D10Device *This,
   const void *pShaderBytecode,
   SIZE_T BytecodeLength,
   ID3D10VertexShader **ppVertexShader);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateGeometryShader(
   ID3D10Device *This,
   const void *pShaderBytecode,
   SIZE_T BytecodeLength,
   ID3D10GeometryShader **ppGeometryShader);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateGeometryShaderWithStreamOutput(
   ID3D10Device *This,
   const void *pShaderBytecode,
   SIZE_T BytecodeLength,
   const D3D10_SO_DECLARATION_ENTRY *pSODeclaration,
   UINT NumEntries,
   UINT OutputStreamStride,
   ID3D10GeometryShader **ppGeometryShader);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreatePixelShader(
   ID3D10Device *This,
   const void *pShaderBytecode,
   SIZE_T BytecodeLength,
   ID3D10PixelShader **ppPixelShader);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateBlendState(
   ID3D10Device *This,
   const D3D10_BLEND_DESC *pBlendStateDesc,
   ID3D10BlendState **ppBlendState);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateDepthStencilState(
   ID3D10Device *This,
   const D3D10_DEPTH_STENCIL_DESC *pDepthStencilDesc,
   ID3D10DepthStencilState **ppDepthStencilState);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateRasterizerState(
   ID3D10Device *This,
   const D3D10_RASTERIZER_DESC *pRasterizerDesc,
   ID3D10RasterizerState **ppRasterizerState);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateSamplerState(
   ID3D10Device *This,
   const D3D10_SAMPLER_DESC *pSamplerDesc,
   ID3D10SamplerState **ppSamplerState);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateQuery(
   ID3D10Device *This,
   const D3D10_QUERY_DESC *pQueryDesc,
   ID3D10Query **ppQuery);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreatePredicate(
   ID3D10Device *This,
   const D3D10_QUERY_DESC *pPredicateDesc,
   ID3D10Predicate **ppPredicate);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateCounter(
   ID3D10Device *This,
   const D3D10_COUNTER_DESC *pCounterDesc,
   ID3D10Counter **ppCounter);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateShaderResourceView1(
   ID3D10Device1 *This,
   ID3D10Resource *pResource,
   const D3D10_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
   ID3D10ShaderResourceView1 **ppSRView);
HRESULT STDMETHODCALLTYPE npt_d3d10_dev_CreateBlendState1(
   ID3D10Device1 *This,
   const D3D10_BLEND_DESC1 *pBlendStateDesc,
   ID3D10BlendState1 **ppBlendState);

#endif
#endif
