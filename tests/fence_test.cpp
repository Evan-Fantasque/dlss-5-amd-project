#include <d3d12.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <thread>
#include <iostream>
#include "Performance.h"
using Microsoft::WRL::ComPtr;
void check(HRESULT h) { if (FAILED(h)) throw h; }
int main()
{
    try {
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i=0; factory->EnumAdapters1(i,&adapter)==S_OK; ++i) {
            DXGI_ADAPTER_DESC1 d; adapter->GetDesc1(&d);
            if (d.VendorId==0x1002) break;
            adapter.Reset();
        }
        if (!adapter) return 2;
        ComPtr<ID3D12Device> d12; check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d12)));
        ComPtr<ID3D11Device> base; ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&base,nullptr,&context));
        ComPtr<ID3D11Device5> d11; check(base.As(&d11));
        ComPtr<ID3D11DeviceContext4> c11; check(context.As(&c11));
        ComPtr<ID3D11Fence> producer; check(d11->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&producer)));
        HANDLE shared=nullptr; check(producer->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&shared));
        ComPtr<ID3D12Fence> consumer;
        const auto opened=d12->OpenSharedHandle(shared,IID_PPV_ARGS(&consumer)); CloseHandle(shared); check(opened);
        for (UINT64 value=1; value<=100; ++value) {
            check(c11->Signal(producer.Get(),value)); c11->Flush();
            check(AmdPreSr::Perf::WaitForInput(consumer.Get(),value,d12.Get()));
            if (consumer->GetCompletedValue()<value) return 3;
        }
        // A delayed producer must never be treated as complete just because a reused event woke.
        ComPtr<ID3D12Fence> delayed; check(d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&delayed)));
        std::thread signal([&] { Sleep(30); delayed->Signal(1); });
        const double start=AmdPreSr::Perf::NowMs();
        const auto waited=AmdPreSr::Perf::WaitForInput(delayed.Get(),1,d12.Get());
        signal.join(); check(waited);
        const double elapsed=AmdPreSr::Perf::NowMs()-start;
        if (elapsed<20 || delayed->GetCompletedValue()!=1) return 4;
        // Deliberately remove a separate software device, never the user's AMD device.
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device5> removed;
        check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&removed)));
        ComPtr<ID3D12Fence> deadFence; check(removed->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&deadFence)));
        removed->RemoveDevice();
        const auto failure=AmdPreSr::Perf::WaitForInput(deadFence.Get(),1,removed.Get());
        if (SUCCEEDED(failure)) return 5;
        // A later healthy device/fence remains usable after the removed-device path.
        check(AmdPreSr::Perf::WaitForInput(consumer.Get(),100,d12.Get()));
        std::cout << "PASS: 100 D3D11 shared fence handoffs, delayed producer " << elapsed
                  << "ms, isolated WARP loss rejected, healthy fence reused\n";
        return 0;
    } catch(HRESULT h) { std::cerr << "HRESULT=" << std::hex << h << '\n'; return 1; }
}
