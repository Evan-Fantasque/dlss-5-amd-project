#include "SplitCommandList.h"
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void ck(HRESULT hr) { if(FAILED(hr))throw std::runtime_error("D3D fixture failure"); }
void require(bool ok,const char* what) { if(!ok)throw std::runtime_error(what); }
int main() try {
    ComPtr<IDXGIFactory4> factory;ck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;ck(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device;ck(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,4};
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    D3D12_ROOT_SIGNATURE_DESC desc{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,errors;ck(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));
    ComPtr<ID3D12RootSignature> root;ck(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    const char shader[]="RWStructuredBuffer<uint> flags:register(u0); cbuffer Params:register(b0){uint mode;uint3 pad;} [numthreads(1,1,1)] void main(){flags[mode]=1;}";
    ck(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"main","cs_5_1",0,0,&blob,&errors));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;ck(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=256;rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> flags;ck(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&flags)));
    ComPtr<AmdPreSr::SplitCommandList> split;split.Attach(new AmdPreSr::SplitCommandList);ck(split->Init(device.Get()));
    void* unknown=reinterpret_cast<void*>(1);
    require(split->QueryInterface(__uuidof(ID3D12GraphicsCommandList1),&unknown)==E_NOINTERFACE && !unknown,"QI must reject extended interfaces");
    require(split->Reset(nullptr,nullptr)==E_NOTIMPL,"Proxy Reset must be rejected");
    ComPtr<IUnknown> identity;ck(split.As(&identity));require(identity.Get()==static_cast<IUnknown*>(split.Get()),"COM identity");
    auto nativeRoot=root.Get();auto nativePipeline=pipeline.Get();auto nativeFlags=flags.Get();
    auto record=[&](int variant) {
        split->Arm(&nativeRoot,&nativePipeline,&nativeFlags);
        split->SetComputeRootSignature(root.Get());split->SetPipelineState(pipeline.Get());
        split->SetComputeRootUnorderedAccessView(0,flags->GetGPUVirtualAddress());
        UINT constants[4]={0,7,2097152,0};
        split->SetComputeRoot32BitConstants(1,4,constants,0);
        if(variant!=2)split->Dispatch(variant==4?2:1,1,1);
        if(variant==3)split->Dispatch(1,1,1);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=flags.Get();
        if(variant!=1)split->ResourceBarrier(1,&barrier);
        constants[0]=variant==5?2:1;
        split->SetComputeRoot32BitConstants(1,4,constants,0);split->Dispatch(1,1,1);
        return split->EndNative();
    };
    for(int passes: {1,3}) {
        ck(split->Begin());for(int i=0;i<passes;++i)require(record(0),"Valid boundary rejected");
        require(split->SegmentCount()==passes+1,"Segment count mismatch");ck(split->Close());
    }
    for(int variant=1;variant<=5;++variant) {
        ck(split->Begin());require(!record(variant),"Malformed boundary accepted");
        require(FAILED(split->Close()),"Malformed recording did not fail closed");
    }
    ck(split->Begin());for(int i=0;i<3;++i)require(record(0),"Valid prefix");
    require(!record(0) && FAILED(split->Close()),"Fourth pass overflow accepted");
    ck(split->Begin());require(record(0),"Reset after rejected recording failed");ck(split->Close());
    std::cout<<"PASS: WARP recording boundaries (1/3 passes), missing barrier/capture, duplicate capture, wrong dimensions/mode, pass overflow, recovery reset, COM identity. No malformed list submitted.\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
