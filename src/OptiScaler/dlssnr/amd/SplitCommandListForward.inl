// Forwarded base D3D12 methods; generated from Windows SDK 10.0.22621.0.
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override
    { return Current()->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override
    { return Current()->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override
    { return Current()->SetPrivateDataInterface(guid, pData); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override
    { return Current()->SetName(Name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppvDevice) override
    { return Current()->GetDevice(riid, ppvDevice); }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType(void) override
    { return Current()->GetType(); }
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *pPipelineState) override
    { Current()->ClearState(pPipelineState); }
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override
    { Current()->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override
    { Current()->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override
    { Current()->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes); }
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc, const D3D12_BOX *pSrcBox) override
    { Current()->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox); }
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource) override
    { Current()->CopyResource(pDstResource, pSrcResource); }
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE *pTileRegionStartCoordinate, const D3D12_TILE_REGION_SIZE *pTileRegionSize, ID3D12Resource *pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override
    { Current()->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags); }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override
    { Current()->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override
    { Current()->IASetPrimitiveTopology(PrimitiveTopology); }
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports) override
    { Current()->RSSetViewports(NumViewports, pViewports); }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D12_RECT *pRects) override
    { Current()->RSSetScissorRects(NumRects, pRects); }
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[ 4 ]) override
    { Current()->OMSetBlendFactor(BlendFactor); }
    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override
    { Current()->OMSetStencilRef(StencilRef); }
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *pCommandList) override
    { Current()->ExecuteBundle(pCommandList); }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature) override
    { Current()->SetGraphicsRootSignature(pRootSignature); }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override
    { Current()->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor); }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override
    { Current()->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override
    { Current()->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override
    { Current()->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues) override
    { Current()->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override
    { Current()->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override
    { Current()->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override
    { Current()->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override
    { Current()->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView) override
    { Current()->IASetIndexBuffer(pView); }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW *pViews) override
    { Current()->IASetVertexBuffers(StartSlot, NumViews, pViews); }
    void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *pViews) override
    { Current()->SOSetTargets(StartSlot, NumViews, pViews); }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor) override
    { Current()->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor); }
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT *pRects) override
    { Current()->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override
    { Current()->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const UINT Values[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override
    { Current()->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const FLOAT Values[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override
    { Current()->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion) override
    { Current()->DiscardResource(pResource, pRegion); }
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override
    { Current()->BeginQuery(pQueryHeap, Type, Index); }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override
    { Current()->EndQuery(pQueryHeap, Type, Index); }
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries, ID3D12Resource *pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) override
    { Current()->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries, pDestinationBuffer, AlignedDestinationBufferOffset); }
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) override
    { Current()->SetPredication(pBuffer, AlignedBufferOffset, Operation); }
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *pData, UINT Size) override
    { Current()->SetMarker(Metadata, pData, Size); }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *pData, UINT Size) override
    { Current()->BeginEvent(Metadata, pData, Size); }
    void STDMETHODCALLTYPE EndEvent(void) override
    { Current()->EndEvent(); }
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset) override
    { Current()->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset); }
