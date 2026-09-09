#include "AmdPreSr.h"
#include <cstring>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <wrl/client.h>
#include "tail_probe.h"
#include "gpu_flags_probe.h"
#include <thread>
using Microsoft::WRL::ComPtr;
void ck(HRESULT hr) {
  if (FAILED(hr))
    throw std::runtime_error("HRESULT " +
                             std::to_string(static_cast<unsigned>(hr)));
}
void barrier(ID3D12GraphicsCommandList *c, ID3D12Resource *r,
             D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER v{};
  v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  v.Transition = {r, 0, a, b};
  c->ResourceBarrier(1, &v);
}
int wmain(int argc, wchar_t **argv) {
  try {
    if (argc < 2)
      return 2;
    UINT passes = argc > 2 ? _wtoi(argv[2]) : 1;
    UINT size = argc > 3 ? _wtoi(argv[3]) : 128;
    UINT active = argc > 4 ? _wtoi(argv[4]) : size;
    UINT depthBits = argc > 5 ? _wtoi(argv[5]) : 0;
    bool queueChanges = argc > 6 && _wtoi(argv[6]) != 0;
    bool resizeEveryFrame = argc > 7 && _wtoi(argv[7]) != 0;
    bool forceTimeout = argc > 8 && _wtoi(argv[8]) != 0;
    bool delayedNotification = argc > 9 && _wtoi(argv[9]) != 0;
    bool varySettings = argc > 10 && _wtoi(argv[10]) != 0;
    float scale = argc > 11 ? float(_wtof(argv[11])) : 1.f;
    bool dependentSignal = argc > 12 && _wtoi(argv[12]) != 0;
    bool varyScale = argc > 13 && _wtoi(argv[13]) != 0;
    bool displayMotion = argc > 14 && _wtoi(argv[14]) != 0;
    bool signedInput = argc > 15 && _wtoi(argv[15]) != 0;
    const int continuityMode = argc > 16 ? _wtoi(argv[16]) : 0;
    const bool timeoutAtExit = argc > 17 && _wtoi(argv[17]) != 0;
    const auto perfIni=(std::filesystem::path(argv[1])/L"amd_presr_perf.ini").wstring();
    const bool phased=GetPrivateProfileIntW(L"Performance",L"PhasedSubmission",0,perfIni.c_str())==1;
    wchar_t frameText[32]{};GetEnvironmentVariableW(L"AMD_TEST_FRAMES",frameText,32);
    const int totalFrames=*frameText ? _wtoi(frameText) : 8;
    const bool captureDependency=GetEnvironmentVariableW(L"AMD_TEST_CAPTURE_DEPENDENCY",nullptr,0)!=0;
    const bool gameShape=GetEnvironmentVariableW(L"AMD_TEST_GAME_SHAPE",nullptr,0)!=0;
    const bool allowRecovery=GetEnvironmentVariableW(L"AMD_TEST_RECOVERY",nullptr,0)!=0;
    const bool preparedControl=GetEnvironmentVariableW(L"AMD_TEST_PREPARED",nullptr,0)!=0;
    auto activeHeight=[&]{return gameShape ? active*1107/1968 : active;};
    const DWORD tailMode = GetEnvironmentVariableW(L"AMD_TEST_TAIL_SYNC", nullptr, 0) ? 1 :
                          GetEnvironmentVariableW(L"AMD_TEST_TAIL_ASYNC", nullptr, 0) ? 2 : 0;
    if (size < 64 || size > 2048 || active < 64 || active > size)
      return 2;
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
      debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;
    ck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      adapter->GetDesc1(&desc);
      if (desc.VendorId == 0x1002 &&
          SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                      IID_PPV_ARGS(&device))))
        break;
      adapter.Reset();
    }
    if (!device)
      throw std::runtime_error("No AMD D3D12 device");
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{};
    ck(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandQueue> secondQueue, presentQueue;
    ck(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&secondQueue)));
    ck(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&presentQueue)));
    ComPtr<ID3D12CommandAllocator> alloc;
    ck(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ck(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
                                 nullptr, IID_PPV_ARGS(&cmd)));
    auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE type,
                      D3D12_RESOURCE_STATES state) {
      ComPtr<ID3D12Resource> r;
      D3D12_HEAP_PROPERTIES hp{};
      hp.Type = type;
      D3D12_RESOURCE_DESC rd{};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      rd.Width = size;
      rd.Height = 1;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = 1;
      rd.SampleDesc.Count = 1;
      rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      ck(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                         nullptr, IID_PPV_ARGS(&r)));
      return r;
    };
    std::vector<ComPtr<ID3D12Resource>> uploads;
    ComPtr<ID3D12DescriptorHeap> depthHeap;
    auto tex = [&](DXGI_FORMAT format, int kind) {
      ComPtr<ID3D12Resource> r;
      D3D12_HEAP_PROPERTIES hp{};
      hp.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC rd{};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      rd.Width = size;
      rd.Height = size;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = 1;
      rd.SampleDesc.Count = 1;
      rd.Format = format;
      if (kind == 2 && depthBits) {
        DXGI_FORMAT view = depthBits == 16 ? DXGI_FORMAT_D16_UNORM :
                           depthBits == 24 ? DXGI_FORMAT_D24_UNORM_S8_UINT :
                           depthBits == 64 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
        rd.Format = depthBits == 16 ? DXGI_FORMAT_R16_TYPELESS :
                    depthBits == 24 ? DXGI_FORMAT_R24G8_TYPELESS :
                    depthBits == 64 ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        ck(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&r)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        ck(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&depthHeap)));
        D3D12_DEPTH_STENCIL_VIEW_DESC vd{};
        vd.Format = view;
        vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        auto handle = depthHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateDepthStencilView(r.Get(), &vd, handle);
        cmd->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
        D3D12_RESOURCE_BARRIER transition{};
        transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition = {r.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        cmd->ResourceBarrier(1, &transition);
        return r;
      }
      ck(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&r)));
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      UINT64 totalBytes;
      device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr,
                                    &totalBytes);
      auto up = buffer(totalBytes, D3D12_HEAP_TYPE_UPLOAD,
                       D3D12_RESOURCE_STATE_GENERIC_READ);
      unsigned char *data;
      ck(up->Map(0, nullptr, reinterpret_cast<void **>(&data)));
      std::memset(data, 0, totalBytes);
      for (UINT y = 0; y < size; ++y)
        for (UINT x = 0; x < size; ++x) {
          auto p = data + y * fp.Footprint.RowPitch;
          if (kind == 0) {
            uint16_t pixel[4] = {static_cast<uint16_t>(0x3000 + ((size>512 ? x%128 : x) * 12)),
                                 static_cast<uint16_t>(0x3400 + (size>512 ? y%128 : y) * 8), 0x3800,
                                 0x3c00};
            if (signedInput) pixel[2] = 0xb800;
            std::memcpy(p + x * 8, pixel, 8);
          } else if (kind == 2) {
            float z = 0.5f;
            std::memcpy(p + x * 4, &z, 4);
          }
        }
      up->Unmap(0, nullptr);
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = r.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = up.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = fp;
      cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      barrier(cmd.Get(), r.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      uploads.push_back(up);
      return r;
    };
    auto colour = tex(DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
    auto motion = tex(DXGI_FORMAT_R16G16_FLOAT, 1);
    auto depth = tex(DXGI_FORMAT_R32_FLOAT, 2);
    auto backend = new AmdPreSr::Backend(device.Get(), queueChanges ? presentQueue.Get() : queue.Get(), argv[1]);
    AmdPreSr::Frame frame{};
    frame.colour = colour.Get();
    frame.motion = motion.Get();
    frame.depth = depth.Get();
    frame.width = active;
    frame.height = activeHeight();
    frame.motionScaleX = (float)active;
    frame.motionScaleY = (float)activeHeight();
    AmdPreSr::Settings settings{};
    settings.passes = passes;
    settings.modelScale = scale;
    if (displayMotion) { frame.motionWidth = size; frame.motionHeight = size; }
    size_t badFrames = 0;
    for (int iteration = 0; iteration < totalFrames; ++iteration) {
      auto submitQueue = queueChanges && (iteration % 4 >= 2) ? secondQueue.Get() : queue.Get();
      if (varySettings) {
        settings.passes = 1 + iteration % passes;
        settings.tone = iteration % 2 ? 0.5f : 0.0f;
        settings.structure = iteration % 2 ? 0.75f : 1.0f;
      }
      if (resizeEveryFrame) {
        active = iteration % 2 == 0 ? size : size / 2;
        frame.width = active;
        frame.height = active;
      } else if (!gameShape && iteration == 4 && active >= 128) {
        active -= 32;
        frame.width = active;
        frame.height = active;
      }
      if (varyScale) { const float scales[] = {1.f, .75f, .5f, .25f}; settings.modelScale = scales[iteration % 4]; }
      frame.reset = (iteration == 5);
      if (continuityMode == 3 && iteration == 2) {
        auto invalid = frame;
        invalid.sourceCall = backend->BeginFrame(true);
        invalid.reset = true;
        invalid.colour = nullptr;
        if (backend->Record(cmd.Get(), invalid, settings))
          throw std::runtime_error("Invalid colour frame was accepted");
        backend->TraceFrame(invalid.sourceCall, "record_rejected");
      }
      frame.sourceCall = backend->BeginFrame(frame.reset);
      if (iteration == 3) backend->InvalidateHistory();
      std::thread captureRelease;
      std::atomic<bool> prematureNotify{false};
      ComPtr<ID3D12Fence> captureBlock;
      if (phased || preparedControl) {
        // Emulate the bridge's completed producer copies and empty outer list.
        ck(cmd->Close());ID3D12CommandList* prefix=cmd.Get();
        submitQueue->ExecuteCommandLists(1,&prefix);
        ComPtr<ID3D12Fence> ready;ck(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ready)));
        ck(submitQueue->Signal(ready.Get(),1));
        HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);ck(ready->SetEventOnCompletion(1,done));
        if(WaitForSingleObject(done,5000)!=WAIT_OBJECT_0)throw std::runtime_error("Producer prefix fence");
        CloseHandle(done);ck(alloc->Reset());ck(cmd->Reset(alloc.Get(),nullptr));
        if(captureDependency && iteration==1) {
          ck(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&captureBlock)));
          ck(submitQueue->Wait(captureBlock.Get(),1));
          auto native=reinterpret_cast<unsigned char*>(GetModuleHandleW(L"dlssnr_amd_pass1.dll"));
          const UINT published=*reinterpret_cast<volatile UINT*>(native+0x76c10);
          captureRelease=std::thread([&,native,published]{
            Sleep(100);
            prematureNotify=*reinterpret_cast<volatile UINT*>(native+0x76c10)!=published;
            captureBlock->Signal(1);
          });
        }
      }
      const auto workStarted=GetTickCount64();
#ifdef AMD_TEST_BASELINE
      if(phased)throw std::runtime_error("Baseline has no phased API");
      auto out=backend->Record(cmd.Get(),frame,settings);
#else
      auto out = phased ? backend->RecordPhased(cmd.Get(),submitQueue,frame,settings)
                        : backend->Record(cmd.Get(), frame, settings);
#endif
      if(captureRelease.joinable()) {
        captureRelease.join();
        if(prematureNotify)throw std::runtime_error("HIP was notified before capture dependency completed");
        std::cout << "capture_dependency notification ordering PASS" << std::endl;
      }
      if (!out && (forceTimeout || allowRecovery) &&
          (backend->Status().find("retry in 1s") != std::string::npos || backend->Status().find("recovery pending")!=std::string::npos)) {
        std::cout << "Cooldown observed; waiting to test recovery" << std::endl;
        backend->TraceFrame(frame.sourceCall, "record_rejected");
        Sleep(1100);
        frame.sourceCall = backend->BeginFrame(frame.reset);
#ifdef AMD_TEST_BASELINE
        out=backend->Record(cmd.Get(),frame,settings);
#else
        out=phased ? backend->RecordPhased(cmd.Get(),submitQueue,frame,settings) : backend->Record(cmd.Get(),frame,settings);
#endif
      }
#ifdef AMD_TEST_CANCEL
      if (!out && backend->Status().find("Phased record rejected")!=std::string::npos) {
        if(backend->Ready())throw std::runtime_error("Rejected backend remained enabled");
        auto native=reinterpret_cast<unsigned char*>(GetModuleHandleW(L"dlssnr_amd_pass1.dll"));
        const auto published=*reinterpret_cast<volatile UINT*>(native+0x76c10);
        const auto complete=*reinterpret_cast<volatile UINT*>(native+0x76c14);
        if(published!=complete)throw std::runtime_error("Rejected job was published");
        if(!backend->Shutdown())throw std::runtime_error("Unsubmitted cancellation retained an idle worker");
        std::cout<<"boundary rejection before submission, disabled retry, safe shutdown PASS at iteration "<<iteration<<std::endl;
        return 0;
      }
#endif
      if (!out)
        throw std::runtime_error(backend->Status());
      if (phased && (backend->Ready() || backend->Shutdown()))
        throw std::runtime_error("Phased resources released before outer consumer submission");
      if (tailMode && iteration == 0) TailProbe::Install();
      backend->TraceFrame(frame.sourceCall, "recorded");
      if (!phased && (varySettings || iteration == 3 || (continuityMode == 1 && iteration == 1) ||
          ((continuityMode == 2 || continuityMode == 3) && iteration == 2))) {
        for (UINT pass = 1; pass <= settings.passes; ++pass) {
          auto name = L"dlssnr_amd_pass" + std::to_wstring(pass) + L".dll";
          auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(name.c_str()));
          if (!base || *(base + 0x765f8) != 0)
            throw std::runtime_error("Temporal history was not invalidated before the next job");
        }
      }
      if (continuityMode == 2 && iteration == 1) {
        if (backend->Ready()) throw std::runtime_error("Unsubmitted job unexpectedly ready");
        // Simulate two bridge calls while the original list is pending. Only
        // the first call has Reset. Both bypass without touching GPU history.
        auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(L"dlssnr_amd_pass1.dll"));
        const auto valid = *(base + 0x765f8);
        auto skipped = backend->BeginFrame(true);
        backend->TraceFrame(skipped, "backend_not_ready");
        skipped = backend->BeginFrame(false);
        backend->TraceFrame(skipped, "backend_not_ready");
        if (*(base + 0x765f8) != valid) throw std::runtime_error("Busy observation mutated GPU history");
      }
      auto desc = out->GetDesc();
      if (desc.Width != active || desc.Height != activeHeight())
          throw std::runtime_error("NR scale changed the SR input extent");
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      UINT64 bytes;
      device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr,
                                    &bytes);
      auto read = buffer(bytes, D3D12_HEAP_TYPE_READBACK,
                         D3D12_RESOURCE_STATE_COPY_DEST);
      barrier(cmd.Get(), out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = read.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = fp;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = out;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      barrier(cmd.Get(), out, D3D12_RESOURCE_STATE_COPY_SOURCE,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      ck(cmd->Close());
      ID3D12CommandList *lists[]{cmd.Get()};
      auto start = GetTickCount64();
      const bool forced = forceTimeout && iteration == (timeoutAtExit ? 7 : 2);
      if (forced && !delayedNotification) {
        // Inject the recovered watchdog abort word, without changing its code.
        for (UINT pass = 1; pass <= settings.passes; ++pass) {
          auto name = L"dlssnr_amd_pass" + std::to_wstring(pass) + L".dll";
          auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(name.c_str()));
          if (!base) throw std::runtime_error("Test runtime not loaded");
          auto abortWord = *reinterpret_cast<volatile LONG**>(base + 0x76c68);
          auto job = *reinterpret_cast<UINT*>(base + 0x76d74);
          if (!abortWord) throw std::runtime_error("Watchdog test word unavailable");
          InterlockedExchange(abortWord, static_cast<LONG>(job));
        }
      }
      // An unrelated presentation submission must not publish our HIP job.
      backend->Submitting(presentQueue.Get(), 0, nullptr);
      backend->Submitted(presentQueue.Get(), 0, nullptr);
      if (!(forced && delayedNotification)) backend->Submitting(submitQueue, 1, lists);
      ComPtr<ID3D12Fence> dependency;
      if (dependentSignal) {
        ck(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dependency)));
        ck(submitQueue->Wait(dependency.Get(), 1));
      }
      submitQueue->ExecuteCommandLists(1, lists);
      if (forced && delayedNotification) {
        ComPtr<ID3D12Fence> boundedFence;
        ck(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&boundedFence)));
        ck(submitQueue->Signal(boundedFence.Get(), 1));
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ck(boundedFence->SetEventOnCompletion(1, done));
        auto result = WaitForSingleObject(done, 1500);
        CloseHandle(done);
        if (result != WAIT_OBJECT_0) throw std::runtime_error("GPU did not exit bounded wait without notification");
        std::cout << "GPU completed before worker notification in " << GetTickCount64()-start << "ms" << std::endl;
      } else if (forced) Sleep(100);
      LARGE_INTEGER qpc0, qpc1, qpf;
      QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&qpc0);
      backend->Submitted(submitQueue, 1, lists);
      if (tailMode && iteration == 1) TailProbe::VerifySubmitted(tailMode == 2);
      QueryPerformanceCounter(&qpc1);
      const double submitMs = 1000.0 * (qpc1.QuadPart - qpc0.QuadPart) / qpf.QuadPart;
      if (dependentSignal) {
        // This CPU signal is deliberately impossible until Submitted returns.
        if(phased && (backend->Ready() || backend->Shutdown()))
          throw std::runtime_error("Phased resources released before blocked outer consumer completed");
        ck(dependency->Signal(1));
        std::cout << "post_return_dependency submitted_cpu_ms=" << submitMs << std::endl;
        if (submitMs > 100) throw std::runtime_error("Submitted blocked the post-return dependency");
      }
      ComPtr<ID3D12Fence> fence;
      ck(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
      ck(submitQueue->Signal(fence.Get(), 1));
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      ck(fence->SetEventOnCompletion(1, event));
      if (WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
        throw std::runtime_error("GPU fence timeout");
      CloseHandle(event);
      const auto workMs=GetTickCount64()-workStarted;
      if (tailMode == 2 && iteration == 1) TailProbe::VerifyAsync(backend);
      if (continuityMode == 1 && iteration == 0) {
        // GPU work has completed, but don't observe backend completion until
        // after a loading-like pause. Next Record must still see the input gap.
        Sleep(700);
      }
      const auto readyStart = GetTickCount64();
      while (!backend->Ready() && GetTickCount64() - readyStart < 6000) Sleep(1);
      if (!backend->Ready()) throw std::runtime_error("Native completion did not retire: " + backend->Status());
      CheckGpuFlags(argv[1],iteration,settings.passes,phased);
      if (dependentSignal && backend->Status().find("timeout events=0") == std::string::npos)
          throw std::runtime_error("Post-return dependency caused a native timeout");
      unsigned char *data;
      ck(read->Map(0, nullptr, reinterpret_cast<void **>(&data)));
      size_t changed = 0, invalid = 0;
      for (UINT y = 0; y < activeHeight(); ++y)
        for (UINT x = 0; x < active; ++x) {
          auto p = reinterpret_cast<uint16_t *>(
              data + y * fp.Footprint.RowPitch + x * 8);
          if (p[0] != (0x3000 + (size>512 ? x%128 : x) * 12) || p[1] != (0x3400 + (size>512 ? y%128 : y) * 8) ||
              p[2] != (signedInput ? 0xb800 : 0x3800))
            ++changed;
          for (int c = 0; c < 3; ++c)
            if ((p[c] & 0x7c00) == 0x7c00)
              ++invalid;
        }
      if (totalFrames<=8 || iteration==totalFrames-1) {
      std::ofstream raw(
          std::filesystem::path(argv[1]) /
              (L"smoke-pass" + std::to_wstring(passes) + L".rgba16f"),
          std::ios::binary);
      for (UINT y = 0; y < activeHeight(); ++y)
        raw.write(reinterpret_cast<char *>(data + y * fp.Footprint.RowPitch),
                  active * 8);
      raw.close();
      }
      read->Unmap(0, nullptr);
      std::cout << "frame=" << iteration << " active=" << active
                << " passes=" << settings.passes << " scale=" << settings.modelScale << " changed_pixels=" << changed
                << " nonfinite=" << invalid << " work_cpu_ms=" << workMs
                << " elapsed_ms=" << GetTickCount64() - start
                << " status=" << backend->Status() << std::endl;
      badFrames += ((forced ? changed != 0 : !allowRecovery && changed == 0) || invalid > 0);
      if (tailMode && iteration == 1) break;
      if (iteration < totalFrames-1) {
        ck(alloc->Reset());
        ck(cmd->Reset(alloc.Get(), nullptr));
      }
    }
    // Stop and join the recovered workers before process teardown.
    ComPtr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info))) {
      for (UINT64 i = 0;
           i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T len = 0;
        info->GetMessage(i, nullptr, &len);
        std::vector<unsigned char> storage(len);
        auto m = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        info->GetMessage(i, m, &len);
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::cerr << m->pDescription << std::endl;
          ++badFrames;
        }
      }
    }
    if (!backend->Shutdown())
      throw std::runtime_error("Worker shutdown was not safe");
    ExitProcess(badFrames == 0 ? 0 : 3);
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    ExitProcess(1);
  }
}
