#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>

namespace AmdPreSr
{
// This COM recorder is passed only to the hash-checked native Record function.
// Never pass the proxy to ExecuteCommandLists: submit Segment(i) instead.
// Each split is BEFORE the native mode=1 wait dispatch, after mode=0 capture
// and its UAV barrier. Segments are submitted separately, with explicit fences.
class SplitCommandList final : public ID3D12GraphicsCommandList
{
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    std::atomic<ULONG> refs {1};
    std::array<Ptr<ID3D12CommandAllocator>,4> allocators;
    std::array<Ptr<ID3D12GraphicsCommandList>,4> lists;
    std::array<bool,4> open {};
    UINT index=0;
    HRESULT error=S_OK;
    ID3D12RootSignature* root=nullptr;
    ID3D12PipelineState* pipeline=nullptr;
    ID3D12RootSignature** nativeRoot=nullptr;
    ID3D12PipelineState** nativePipeline=nullptr;
    ID3D12Resource** nativeFlags=nullptr;
    UINT captureDispatches=0,waitDispatches=0,mode=UINT_MAX;
    bool captureBarrier=false,split=false;
    D3D12_GPU_VIRTUAL_ADDRESS flagsAddress=0,abortAddress=0;
    std::array<ID3D12DescriptorHeap*,2> heaps {};
    UINT heapCount=0;
    bool Native() const
    { return nativeRoot && *nativeRoot && root==*nativeRoot && nativePipeline && pipeline==*nativePipeline; }
    void Error(HRESULT hr=E_UNEXPECTED) { if (SUCCEEDED(error)) error=hr; }
    ID3D12GraphicsCommandList* Current() const { return lists[index].Get(); }
    void Cut()
    {
        if (!captureBarrier || captureDispatches!=1 || split || index==3 ||
            !nativeFlags || !*nativeFlags || !flagsAddress)
        { Error(); return; }
        auto hr=Current()->Close();open[index]=false;
        if (FAILED(hr)) { Error(hr); return; }
        ++index;split=true;
        // Buffer states decay to COMMON at an ExecuteCommandLists boundary.
        // Restore UAV access only when applying AFTER HIP has completed.
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={*nativeFlags,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        Current()->ResourceBarrier(1,&barrier);
        Current()->SetComputeRootSignature(root);
        Current()->SetPipelineState(pipeline);
        if (heapCount) Current()->SetDescriptorHeaps(heapCount,heaps.data());
        Current()->SetComputeRootUnorderedAccessView(0,flagsAddress);
        if (abortAddress) Current()->SetComputeRootShaderResourceView(2,abortAddress);
    }
  public:
    Ptr<ID3D12Fence> fence;
    UINT64 serial=0;
    HRESULT Init(ID3D12Device* device)
    {
        auto hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
        if (FAILED(hr)) return hr;
        for (UINT i=0;i<4;++i)
        {
            hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[i]));
            if (FAILED(hr)) return hr;
            hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[i].Get(),nullptr,IID_PPV_ARGS(&lists[i]));
            if (FAILED(hr)) return hr;
            hr=lists[i]->Close();if (FAILED(hr)) return hr;
        }
        return S_OK;
    }
    // Owner must first retire BOTH native work and the outer consumer list.
    HRESULT Begin()
    {
        index=0;error=S_OK;root=nullptr;pipeline=nullptr;nativeRoot=nullptr;
        nativePipeline=nullptr;nativeFlags=nullptr;heapCount=0;
        for (UINT i=0;i<4;++i)
        {
            if(open[i]) { auto hr=lists[i]->Close();open[i]=false;if(FAILED(hr))return hr; }
            auto hr=allocators[i]->Reset();if(FAILED(hr))return hr;
            hr=lists[i]->Reset(allocators[i].Get(),nullptr);if(FAILED(hr))return hr;
            open[i]=true;
        }
        return S_OK;
    }
    void Arm(ID3D12RootSignature** rs,ID3D12PipelineState** ps,ID3D12Resource** flags)
    { nativeRoot=rs;nativePipeline=ps;nativeFlags=flags;captureDispatches=waitDispatches=0;
      captureBarrier=split=false;mode=UINT_MAX; }
    bool EndNative()
    {
        if(captureDispatches!=1 || waitDispatches!=1 || !split)Error();
        nativeRoot=nullptr;nativePipeline=nullptr;nativeFlags=nullptr;
        return SUCCEEDED(error);
    }
    UINT SegmentCount() const { return index+1; }
    ID3D12CommandList* Segment(UINT i) const { return lists.at(i).Get(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override
    {
        if(!out)return E_POINTER;*out=nullptr;
        if(id!=__uuidof(IUnknown) && id!=__uuidof(ID3D12Object) && id!=__uuidof(ID3D12DeviceChild) &&
           id!=__uuidof(ID3D12CommandList) && id!=__uuidof(ID3D12GraphicsCommandList))return E_NOINTERFACE;
        *out=static_cast<ID3D12GraphicsCommandList*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto n=--refs;if(!n)delete this;return n; }
    HRESULT STDMETHODCALLTYPE Close() override
    {
        for(UINT i=0;i<4;++i)if(open[i]){auto hr=lists[i]->Close();open[i]=false;if(FAILED(hr))Error(hr);}
        return error;
    }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator*,ID3D12PipelineState*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* ps) override
    { pipeline=ps;Current()->SetPipelineState(ps); }
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* rs) override
    { root=rs;flagsAddress=abortAddress=0;mode=UINT_MAX;Current()->SetComputeRootSignature(rs); }
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n,ID3D12DescriptorHeap* const* h) override
    { if(n>2){Error();return;}heapCount=n;for(UINT i=0;i<n;++i)heaps[i]=h[i];Current()->SetDescriptorHeaps(n,h); }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT slot,D3D12_GPU_VIRTUAL_ADDRESS address) override
    { if(slot==0)flagsAddress=address;Current()->SetComputeRootUnorderedAccessView(slot,address); }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT slot,D3D12_GPU_VIRTUAL_ADDRESS address) override
    { if(slot==2)abortAddress=address;Current()->SetComputeRootShaderResourceView(slot,address); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT slot,UINT n,const void* data,UINT offset) override
    {
        if(Native() && slot==1 && n==4 && offset==0 && data)
        { mode=*static_cast<const UINT*>(data);if(mode==1)Cut();else if(mode!=0)Error(); }
        Current()->SetComputeRoot32BitConstants(slot,n,data,offset);
    }
    void STDMETHODCALLTYPE Dispatch(UINT x,UINT y,UINT z) override
    {
        if(Native())
        { if(x!=1 || y!=1 || z!=1)Error();
          if(mode==0)++captureDispatches;else if(mode==1)++waitDispatches;else Error(); }
        Current()->Dispatch(x,y,z);
    }
    void STDMETHODCALLTYPE ResourceBarrier(UINT n,const D3D12_RESOURCE_BARRIER* barriers) override
    {
        if(nativeFlags && captureDispatches==1 && !split)
            for(UINT i=0;i<n;++i)
                if(barriers[i].Type==D3D12_RESOURCE_BARRIER_TYPE_UAV && barriers[i].UAV.pResource==*nativeFlags)
                    captureBarrier=true;
        Current()->ResourceBarrier(n,barriers);
    }
#include "SplitCommandListForward.inl"
};
}
