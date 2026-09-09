#include "AmdPreSr.h"
#include "RuntimeHash.h"
#include "Performance.h"
#include "Continuity.h"
#include "StageDiagnostics.h"
#include "WorkerCompletion.h"
#include "SplitCommandList.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <cmath>

using Microsoft::WRL::ComPtr;
namespace AmdPreSr
{
namespace
{
template <class T> T& At(HMODULE h, size_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(h) + rva); }
void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned>(hr)));
}
void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (!r || a == b)
        return;
    D3D12_RESOURCE_BARRIER v {};
    v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    c->ResourceBarrier(1, &v);
}
struct Packet
{
    ID3D12GraphicsCommandList* list;
    ID3D12Resource* colour;
    UINT colourState, pad14;
    ID3D12Resource* motion;
    UINT motionState, pad24;
    ID3D12Resource* depth;
    UINT depthState, pad34;
    ID3D12Resource* exposure;
    UINT exposureState;
    float scaleX, scaleY;
    UINT pad4c;
};
static_assert(sizeof(Packet) == 0x50 && offsetof(Packet, scaleX) == 0x44);
using InitFn = bool(__fastcall*)(void*, const std::string*);
using RecordFn = void(__fastcall*)(Packet*);
using NotifyFn = void(__fastcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using HipSetFn = int (*)(int);
constexpr char CopyShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; uint sourceW; uint sourceH; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h)return;
 if(w==sourceW && h==sourceH){dst[p.xy]=src.Load(int3(p.xy,0));return;}
 // Integrate the entire source pixel footprint. A single bilinear sample aliases
 // narrow emissive lines when the model runs far below the input resolution.
 float2 lo=float2(p.xy)*float2(sourceW,sourceH)/float2(w,h);
 float2 hi=float2(p.xy+1)*float2(sourceW,sourceH)/float2(w,h);
 int2 first=int2(floor(lo)); float4 sum=0;float total=0;
 [loop]for(int y=first.y;y<int(ceil(hi.y));++y)
 [loop]for(int x=first.x;x<int(ceil(hi.x));++x){
  float2 coverage=max(0,min(hi,float2(x+1,y+1))-max(lo,float2(x,y)));
  float weight=coverage.x*coverage.y;
  sum+=src.Load(int3(clamp(int2(x,y),0,int2(sourceW-1,sourceH-1)),0))*weight;total+=weight;
 }
 dst[p.xy]=sum/max(total,1e-6);
})";
constexpr char ResolveShader[] = R"(
Texture2D<float4> src:register(t0);
Texture2D<float4> baseline:register(t1);
Texture2D<float4> edited:register(t2);
RWTexture2D<float4> dst:register(u0);
cbuffer Extent:register(b0){uint w,h,lowW,lowH;};
float3 delta(int2 p){p=clamp(p,0,int2(lowW-1,lowH-1));return edited.Load(int3(p,0)).rgb-baseline.Load(int3(p,0)).rgb;}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){
 if(p.x>=w||p.y>=h)return;
 float2 q=(float2(p.xy)+.5)*float2(lowW,lowH)/float2(w,h)-.5;
 int2 a=int2(floor(q));float2 t=frac(q);
 float3 d=lerp(lerp(delta(a),delta(a+int2(1,0)),t.x),lerp(delta(a+int2(0,1)),delta(a+1),t.x),t.y);
 float4 c=src.Load(int3(p.xy,0));
 // A reduced neural pixel mixes surfaces and small emitters. Suppress its edit
 // where the original pixel disagrees with that footprint, rather than spreading
 // the edit blindly across high-contrast edges. No previous frame is reused.
 int2 hi=int2(lowW-1,lowH-1);
 float3 b=lerp(lerp(baseline.Load(int3(clamp(a,0,hi),0)).rgb,baseline.Load(int3(clamp(a+int2(1,0),0,hi),0)).rgb,t.x),
 lerp(baseline.Load(int3(clamp(a+int2(0,1),0,hi),0)).rgb,baseline.Load(int3(clamp(a+1,0,hi),0)).rgb,t.x),t.y);
 float3 magnitude=max(max(abs(c.rgb),abs(b)),1e-5);
 float mismatch=max(abs(c.r-b.r)/magnitude.r,max(abs(c.g-b.g)/magnitude.g,abs(c.b-b.b)/magnitude.b));
 float confidence=1-smoothstep(.15,.75,mismatch);
 // Keep extreme low-resolution edits bounded relative to the current footprint.
 float3 limit=.5*max(abs(b),abs(c.rgb));
 d=clamp(d,-limit,limit)*confidence;
 // No neural edit (including watchdog fallback) must preserve signed input.
 if(all(d==0)){dst[p.xy]=c;return;}
 dst[p.xy]=float4(clamp(c.rgb+d,0,65504),c.a);
})";
constexpr char DepthShader[] = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; uint sourceW; uint sourceH; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x<w && p.y<h) {
 uint2 q=min(uint2((float2(p.xy)+.5)*float2(sourceW,sourceH)/float2(w,h)),uint2(sourceW-1,sourceH-1));
 dst[p.xy]=src.Load(int3(q,0)); }
})";
constexpr char MotionShader[] = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; uint sourceW; uint sourceH; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h) return;
 uint2 q=min(uint2((float2(p.xy)+0.5)*float2(sourceW,sourceH)/float2(w,h)),uint2(sourceW-1,sourceH-1));
 // Keep the sampled vector unchanged; convert its pixel scale in the packet.
 dst[p.xy]=src.Load(int3(q,0));
})";
constexpr char ExposureShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; float preExposure; float exposureScale; };
[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID) {
 float e=src.Load(int3(0,0,0)).r*exposureScale/preExposure;
 dst[uint2(0,0)]=isfinite(e) && e>0 ? e : 1.0;
})";
DXGI_FORMAT DepthReadFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_FLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
std::string Layout(ID3D12Resource* resource)
{
    auto d = resource->GetDesc();
    return std::to_string(d.Width) + "x" + std::to_string(d.Height) + " format=" + std::to_string(d.Format) +
           " flags=" + std::to_string(d.Flags) + " samples=" + std::to_string(d.SampleDesc.Count) +
           " array=" + std::to_string(d.DepthOrArraySize) + " dimension=" + std::to_string(d.Dimension);
}
bool HashMatches(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    if (data.size() != 7156224)
        return false;
    BCRYPT_ALG_HANDLE alg {};
    unsigned char digest[32] {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    auto result = BCryptHash(alg, nullptr, 0, data.data(), static_cast<ULONG>(data.size()), digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result >= 0 && std::memcmp(digest, AmdRuntimeSha256, 32) == 0;
}
DXGI_FORMAT ReadFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return f;
    }
}
} // namespace
struct Backend::Impl
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> colour, scaleBaseline, scaleOutput;
    ComPtr<ID3D12PipelineState> resolvePipeline;
    ComPtr<ID3D12Resource> motionCrop, depthCrop;
    ComPtr<ID3D12Resource> exposureCopy;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12PipelineState> depthPipeline;
    ComPtr<ID3D12PipelineState> motionPipeline, exposurePipeline;
    UINT lastMotionWidth = 0, lastMotionHeight = 0;
    UINT lastInputWidth = 0, lastInputHeight = 0;
    bool hadExposure = false;
    std::array<HMODULE, 3> runtime {};
    std::array<UINT, 3> jobs {};
    std::array<UINT, 3> observedTimeouts {};
    std::array<UINT, 3> workerTailPolls {};
    std::filesystem::path directory;
    std::string status = "AMD pre-SR: not initialized";
    std::atomic<ID3D12CommandList*> pending { nullptr };
    std::atomic<bool> failed { false };
    std::atomic<UINT64> resetGeneration { 1 };
    UINT64 consumedResetGeneration = 0;
    Continuity continuity;
    UINT64 activeSourceCall = 0, resetConsumedCall = 0;
    unsigned consumedReasons = 0;
    UINT64 bridgeRecorded = 0, bridgeBypassed = 0;
    const char* lastRecordReason = "record_rejected";
    Settings lastSettings {};
    bool haveSettings = false;
    std::atomic<UINT64> completion { 0 };
    UINT64 frames = 0, serial = 0;
    UINT64 lastSubmitted = 0, lastCompleted = 0, completedFrames = 0;
    UINT64 pendingSkips = 0, fenceSkips = 0, fenceRecoveries = 0;
    UINT64 retryAfter = 0, timeoutEvents = 0;
    bool resetAfterTimeout = false;
    bool firstPublished = false;
    Perf::Options perf;
    Perf::TimerResolution timerResolution;
    Perf::Csv timing, decisions, phaseTiming;
    ComPtr<SplitCommandList> phases;
    std::mutex phasedLock;
    bool phasedAwaitOuter = false;
    StageDiagnostics diagnostics;
    double submittedMs = 0, notifyMs = 0;
    ComPtr<ID3D12GraphicsCommandList> retainedList;
    std::array<ComPtr<ID3D12Resource>, 4> retainedInputs;
    bool asyncSingle = false;
    UINT64 asyncStart = 0;
    UINT width = 0, height = 0, activePasses = 0, lastPasses = 0;
    HipSetFn hipSet = nullptr;
    int hipDevice = -1;
    std::mutex lock;
    void Log(const std::string& s)
    {
        status = s;
        std::ofstream out(directory / L"amd_presr.log", std::ios::app);
        out << GetTickCount64() << " " << s << '\n';
    }
    void ReleaseInputs()
    {
        retainedList.Reset();
        for (auto& resource : retainedInputs)
            resource.Reset();
    }
    // Only before ANY private segment has been submitted. A native record can
    // reserve a job before boundary validation fails; it must not leave an
    // unpublished job preventing safe worker shutdown.
    void CancelUnsubmittedPhased()
    {
        if (!phases || (pending.load() && pending.load()!=phases.Get())) return;
        for (UINT i=0;i<runtime.size();++i)
            if (auto h=runtime[i]; h && At<ID3D12CommandList*>(h,0x76d68)==phases.Get())
            {
                At<ID3D12CommandList*>(h,0x76d68)=nullptr;
                InterlockedExchange(&At<LONG>(h,0x76d70),-1);
                const auto completed=WorkerCompletion::Read(h).inference;
                At<UINT>(h,0x76d74)=completed;
                jobs[i]=completed;
            }
        pending.store(nullptr,std::memory_order_release);
        phasedAwaitOuter=false;
        ReleaseInputs();
    }
    bool WorkerRetired(UINT pass)
    {
        if (!runtime[pass] || !jobs[pass]) return true;
        const auto state = WorkerCompletion::Read(runtime[pass]);
        if (state.inference >= jobs[pass] && !state.Complete(jobs[pass])) ++workerTailPolls[pass];
        return state.Complete(jobs[pass]);
    }
    bool WorkersRetired()
    {
        for (UINT i = 0; i < activePasses; ++i)
            if (!WorkerRetired(i)) return false;
        return true;
    }
    void TimingRow(UINT pass, bool asynchronous, UINT polls = 0, double sleep = 0, double maxSleep = 0)
    {
        if (!perf.timing)
            return;
        auto h = runtime[pass];
        // Completion is observed on the next poll. This interval is an upper
        // bound, not GPU inference time. The private 0x76c40 field is uncalibrated.
        timing.Row(frames, activeSourceCall, pass + 1, jobs[pass], asynchronous, notifyMs, Perf::NowMs() - submittedMs, polls, sleep,
                   maxSleep, At<float>(h, 0x76c40), At<UINT>(h, 0x76c18), !failed.load(),
                   WorkerCompletion::Read(h).inference, WorkerCompletion::Read(h).retired, workerTailPolls[pass]);
    }
    void Stage(UINT pass, const char* phase, bool force = false)
    {
        if (!diagnostics.Active()) return;
        auto h = runtime[pass];
        if (!h) return;
        auto read = [&](size_t rva) { return *reinterpret_cast<volatile UINT*>(reinterpret_cast<uintptr_t>(h) + rva); };
        auto abort = At<volatile LONG*>(h, 0x76c68);
        diagnostics.Observe(pass, jobs[pass], read(0x76c14), read(0x76c18),
                            abort ? static_cast<UINT>(*abort) : 0, At<float>(h, 0x76c40), phase, force);
    }
    void FinishDiagnostics()
    {
        for (UINT i = 0; i < activePasses; ++i) Stage(i, "retire", true);
        diagnostics.Collect();
    }
    // Also called at retirement/shutdown, so a last-job timeout cannot vanish
    // merely because the game never calls Record again.
    bool CollectTimeouts()
    {
        bool changed = false;
        for (UINT i = 0; i < runtime.size(); ++i)
            if (auto h = runtime[i])
            {
                const UINT count = static_cast<UINT>(InterlockedCompareExchange(
                    reinterpret_cast<volatile LONG*>(&At<UINT>(h, 0x76c18)), 0, 0));
                if (count > observedTimeouts[i])
                {
                    timeoutEvents += count - observedTimeouts[i];
                    changed = true;
                }
                observedTimeouts[i] = count;
            }
        if (changed)
        {
            retryAfter = GetTickCount64() + 1000;
            resetAfterTimeout = true;
            Log("AMD timeout: current input preserved; retry in 1s with fresh history. Events=" +
                std::to_string(timeoutEvents));
            timing.Flush();
            decisions.Flush();
        }
        return changed;
    }
    // Called with lock held. Keep every borrowed resource alive until BOTH
    // native inference and the actual D3D12 submission have retired.
    void RetireSingle()
    {
        if (!asyncSingle)
            return;
        if (FAILED(device->GetDeviceRemovedReason()))
        {
            if (!failed.exchange(true))
                Log("AMD stopped: device removed during asynchronous completion; resources retained");
            return;
        }
        auto done = static_cast<UINT>(
            InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&At<UINT>(runtime[0], 0x76c14)), 0, 0));
        Stage(0, "async_observe");
        if (!WorkerRetired(0) || fence->GetCompletedValue() < completion.load())
        {
            if (!failed && GetTickCount64() - asyncStart > 5000)
            {
                failed = true;
                Log("AMD stopped: asynchronous capture/completion exceeded 5 seconds; resources retained; native=" +
                    std::to_string(done) + "/" + std::to_string(jobs[0]) +
                    " fence=" + std::to_string(fence->GetCompletedValue()) + "/" + std::to_string(completion.load()));
            }
            return;
        }
        FinishDiagnostics();
        TimingRow(0, true);
        ReleaseInputs();
        asyncSingle = false;
        pending.store(nullptr, std::memory_order_release);
        const bool timedOut = CollectTimeouts();
        if (!failed && !timedOut)
        {
            ++completedFrames;
            lastCompleted = GetTickCount64();
            status = "Completed AMD pre-SR passes=1 at " + std::to_string(width) + "x" + std::to_string(height);
            if (completedFrames <= 3 || completedFrames % 120 == 0)
                Log(status);
        }
    }
    void InitHip()
    {
        if (hipSet)
            return;
        HMODULE hip = LoadLibraryExW(L"amdhip64_7.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!hip)
            throw std::runtime_error("Cannot load amdhip64_7.dll; Windows error=" + std::to_string(GetLastError()) +
                                     ". Install the compatible AMD HIP 7 runtime; HIP 6 alone is insufficient.");
        wchar_t hipPath[MAX_PATH] {};
        GetModuleFileNameW(hip, hipPath, MAX_PATH);
        Log("HIP runtime: " + std::filesystem::path(hipPath).string());
        auto count = reinterpret_cast<int (*)(int*)>(GetProcAddress(hip, "hipGetDeviceCount"));
        auto props = reinterpret_cast<int (*)(void*, int)>(GetProcAddress(hip, "hipGetDevicePropertiesR0600"));
        hipSet = reinterpret_cast<HipSetFn>(GetProcAddress(hip, "hipSetDevice"));
        if (!count || !props || !hipSet)
            throw std::runtime_error("HIP R0600 API unavailable");
        int n = 0;
        int countResult = count(&n);
        if (countResult != 0 || n == 0)
            throw std::runtime_error("HIP device enumeration failed: code=" + std::to_string(countResult) +
                                     " devices=" + std::to_string(n));
        auto luid = device->GetAdapterLuid();
        for (int i = 0; i < n; ++i)
        {
            // R0600 prefix: name[256], uuid[16], luid[8]. Oversized aligned storage.
            alignas(16) std::array<unsigned char, 8192> p {};
            int propResult = props(p.data(), i);
            Log("HIP candidate " + std::to_string(i) + " code=" + std::to_string(propResult) +
                " name=" + std::string(reinterpret_cast<char*>(p.data())));
            if (propResult == 0 && std::memcmp(p.data() + 272, &luid, 8) == 0)
            {
                hipDevice = i;
                Log("HIP adapter: " + std::string(reinterpret_cast<char*>(p.data())));
                break;
            }
        }
        if (hipDevice < 0 || hipSet(hipDevice) != 0)
            throw std::runtime_error("No HIP adapter matches D3D12 LUID");
    }
    void InitPass(UINT i)
    {
        if (runtime[i])
            return;
        if (perf.timerResolution && !timerResolution.Start())
            Log("Performance: 1ms timer request unavailable");
        InitHip();
        auto path = directory / (L"dlssnr_amd_pass" + std::to_wstring(i + 1) + L".dll");
        if (!HashMatches(path))
            throw std::runtime_error("Private AMD runtime hash mismatch: pass " + std::to_string(i + 1));
        auto weights = directory / L"dlssnr_on_amd_weights.bin";
        if (!std::filesystem::exists(weights))
            throw std::runtime_error("dlssnr_on_amd_weights.bin is required");
        HMODULE h =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!h)
            throw std::runtime_error("Private AMD runtime LoadLibrary failed: " + std::to_string(GetLastError()));
        HMODULE pinned {};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(h), &pinned);
        // Retain module even on failure: CRT registered HIP kernels; no unsafe unloading.
        runtime[i] = h;
        At<ID3D12Device*>(h, 0x764c8) = device.Get();
        device->AddRef();
        At<ID3D12CommandQueue*>(h, 0x764d0) = queue.Get();
        queue->AddRef();
        At<int>(h, 0x76f20) = hipDevice;
        At<uint8_t>(h, 0x76be0) = 1; // configured inline; 0x76be1 is staging state
        At<uint8_t>(h, 0x76c8c) = 1; // external-memory interop
        At<uint8_t>(h, 0x76e1c) = 1; // enabled
        At<uint8_t>(h, 0x76e1e) = 1; // FSR inputs, no swapchain fallback
        At<uint8_t>(h, 0x76e1f) = 1; // depth
        At<int>(h, 0x76e20) = -1;    // auto tonemap by input format
        std::string file = weights.string();
        if (hipSet(hipDevice) != 0 || !reinterpret_cast<InitFn>(reinterpret_cast<uintptr_t>(h) + 0x12380)(
                                          reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(h) + 0x764d8), &file))
            throw std::runtime_error("AMD engine initialization failed");
        At<uint8_t>(h, 0x767f8) = 1;
        Log("Initialized independent AMD pass " + std::to_string(i + 1));
    }
    void InitShader()
    {
        if (root)
            return;
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 };
        D3D12_ROOT_PARAMETER params[3] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 24 };
        D3D12_DESCRIPTOR_RANGE residualRange { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, 0 };
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = { 1, &residualRange };
        D3D12_ROOT_SIGNATURE_DESC desc { 3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, error;
        Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
              "Root signature serialize");
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
              "Root signature create");
        Check(D3DCompile(CopyShader, sizeof(CopyShader), "AMD active crop", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Crop shader compile");
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps {};
        ps.pRootSignature = root.Get();
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline)), "Crop pipeline");
        Check(D3DCompile(DepthShader, sizeof(DepthShader), "AMD depth conversion", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Depth shader compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&depthPipeline)), "Depth pipeline");
        Check(D3DCompile(MotionShader, sizeof(MotionShader), "AMD motion resample", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Motion shader compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&motionPipeline)), "Motion pipeline");
        Check(D3DCompile(ExposureShader, sizeof(ExposureShader), "AMD exposure conversion", nullptr, nullptr, "main",
                         "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Exposure shader compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&exposurePipeline)), "Exposure pipeline");
        Check(D3DCompile(ResolveShader, sizeof(ResolveShader), "AMD residual resolve", nullptr, nullptr, "main",
                         "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Resolve compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&resolvePipeline)), "Resolve pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 14,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "Crop heap");
    }
};
Backend::Backend(ID3D12Device* d, ID3D12CommandQueue* q, const std::filesystem::path& dir) : p(new Impl)
{
    p->device = d;
    p->queue = q;
    p->directory = dir;
    p->perf = Perf::Options::Read(dir);
    if (p->perf.timing)
    {
        p->timing.Open(dir / (L"amd_presr_timing_test8_" + std::to_wstring(GetCurrentProcessId()) + L".csv"),
                       "frame,source_call,pass,job,async_completion,notify_cpu_ms,submission_to_observed_completion_ms,poll_calls,"
                       "poll_sleep_ms,poll_sleep_max_ms,runtime_field_76c40_unverified,runtime_timeouts,device_ok,native_done,worker_done,worker_tail_polls");
        p->decisions.Open(dir / (L"amd_presr_decisions_test8_" + std::to_wstring(GetCurrentProcessId()) + L".csv"),
                          "tick_ms,source_call,decision,recorded_jobs,latest_job,reset_pending_mask,reset_consumed_mask,"
                          "reset_requested_generation,reset_consumed_generation,native_done,native_timeouts,"
                          "submission_pending,fence_completed,fence_target,last_input_w,last_input_h,model_w,model_h,worker_done");
    }
    if (p->perf.timing)
        p->phaseTiming.Open(dir / (L"amd_presr_phased_test8_" + std::to_wstring(GetCurrentProcessId()) + L".csv"),
                            "tick_ms,source_call,frame,pass,phase,segment_count,capture_wait_cpu_ms,worker_wait_cpu_ms,"
                            "apply_wait_cpu_ms,total_cpu_ms,capture_fence_target,capture_fence_completed,native_done,worker_done");
    const bool diagnosticReady = p->diagnostics.Init(d, dir, p->perf.timing && p->perf.diagnosticStages);
    p->Log("Stage diagnostics requested=" + std::to_string(p->perf.diagnosticStages) + " ready=" + std::to_string(diagnosticReady));
    p->Log("Performance test8: async single=" + std::to_string(p->perf.asyncSingle) +
           " timer1ms=" + std::to_string(p->perf.timerResolution) + " timing=" + std::to_string(p->perf.timing));
    try
    {
        Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->fence)), "Completion fence");
    }
    catch (const std::exception& e)
    {
        p->failed = true;
        p->Log(e.what());
    }
}
ID3D12Resource* Backend::Record(ID3D12GraphicsCommandList* cmd, const Frame& incoming, const Settings& cfg)
{
    return RecordImpl(cmd, incoming, cfg, nullptr);
}
ID3D12Resource* Backend::RecordImpl(ID3D12GraphicsCommandList* cmd, const Frame& incoming, const Settings& cfg,
                                  SplitCommandList* split)
{
    std::lock_guard guard(p->lock);
    Frame f = incoming;
    if (!f.sourceCall)
        f.sourceCall = p->continuity.Observe(GetTickCount64(), f.reset);
    p->lastRecordReason = "invalid_input";
    p->RetireSingle();
    if (p->failed || !cmd || !f.colour || !f.motion || !f.depth)
        return nullptr;
    const auto deviceStatus = p->device->GetDeviceRemovedReason();
    if (FAILED(deviceStatus))
    {
        p->failed = true;
        p->Log("AMD stopped: D3D12 device lost, HRESULT=" + std::to_string(static_cast<UINT>(deviceStatus)));
        return nullptr;
    }
    if (p->pending.load())
    {
        p->lastRecordReason = "submission_pending";
        if (++p->pendingSkips <= 3 || p->pendingSkips % 120 == 0)
            p->Log("AMD skipped: previous neural submission still pending; count=" + std::to_string(p->pendingSkips));
        return nullptr;
    }
    const auto completion = p->completion.load();
    if (p->fence->GetCompletedValue() < completion)
    {
        p->lastRecordReason = "gpu_in_flight";
        if (++p->fenceSkips <= 3 || p->fenceSkips % 120 == 0)
            p->Log("AMD skipped: submitted GPU work still in flight; count=" + std::to_string(p->fenceSkips));
        return nullptr;
    }
    if (!p->WorkersRetired())
    {
        p->lastRecordReason = "worker_bookkeeping_pending";
        return nullptr;
    }
    p->FinishDiagnostics();
    p->ReleaseInputs();
    p->CollectTimeouts();
    if (GetTickCount64() < p->retryAfter)
    {
        p->lastRecordReason = "timeout_cooldown";
        return nullptr;
    }
    p->lastRecordReason = "record_rejected";
    try
    {
        // Reject transient/dummy guides before any GPU commands or native jobs.
        // A later valid frame must be allowed to recover without restarting.
        const auto cd = f.colour->GetDesc();
        const UINT iw = f.width ? f.width : UINT(cd.Width), ih = f.height ? f.height : cd.Height;
        for (auto guide : { f.motion, f.depth })
        {
            auto gd = guide->GetDesc();
            if (gd.Width < iw || gd.Height < ih || gd.SampleDesc.Count != 1 || gd.DepthOrArraySize != 1 ||
                gd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            {
                const std::string reason = "AMD neural: waiting for valid full-size guides; received " + Layout(guide);
                if (p->status != reason)
                    p->Log(reason);
                p->resetGeneration.fetch_add(1);
                p->lastRecordReason = "invalid_guides";
                return nullptr;
            }
        }
        auto desc = f.colour->GetDesc();
        UINT w = f.width ? f.width : static_cast<UINT>(desc.Width), h = f.height ? f.height : desc.Height;
        if (p->frames == 0 || p->lastInputWidth != w || p->lastInputHeight != h)
        {
            p->Log("Input active=" + std::to_string(w) + "x" + std::to_string(h) + " colour=" + Layout(f.colour));
            p->Log("Input motion=" + Layout(f.motion) + " depth=" + Layout(f.depth));
        }
        if (!w || !h || w > desc.Width || h > desc.Height || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 ||
            desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            throw std::runtime_error("Unsupported active colour extent/layout");
        // Display-resolution vectors are resampled, never cropped as if they
        // belonged to the render-resolution pixel grid.
        for (auto guide : { f.motion, f.depth })
        {
            auto gd = guide->GetDesc();
            if (gd.Width < w || gd.Height < h || gd.SampleDesc.Count != 1 || gd.DepthOrArraySize != 1 ||
                gd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                throw std::runtime_error(std::string("Unsupported AMD pre-SR ") +
                                         (guide == f.motion ? "motion: " : "depth: ") + Layout(guide));
        }
        const UINT inputW = w, inputH = h;
        const float scale = std::isfinite(cfg.modelScale) ? std::clamp(cfg.modelScale, .25f, 1.f) : 1.f;
        w = (std::min)(inputW, (std::max)(32u, UINT(std::lround(inputW * scale))));
        h = (std::min)(inputH, (std::max)(32u, UINT(std::lround(inputH * scale))));
        const bool scaled = w != inputW || h != inputH;
        const UINT mvW = f.motionWidth ? f.motionWidth : inputW, mvH = f.motionHeight ? f.motionHeight : inputH;
        const auto depthDesc = f.depth->GetDesc();
        // The private AMD runtime already accepts typeless/depth-stencil guides
        // and stages only the colour-sized active region. Preparing another
        // crop/conversion on the game's command list duplicates that work and
        // invalidates some UE 4.26 command lists (Stellar Blade reports
        // E_INVALIDARG from Close). Pass the original guides through instead.
        const bool convertDepth = scaled;
        if (scaled && (depthDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::runtime_error("NR scale: depth is not shader readable; use 100%");
        if (scaled && DepthReadFormat(depthDesc.Format) == DXGI_FORMAT_UNKNOWN)
            throw std::runtime_error("NR scale: unsupported depth view; use 100%");
        const bool resampleMotion = mvW != w || mvH != h;
        const auto motionDesc = f.motion->GetDesc();
        if (resampleMotion && (f.motionWidth > motionDesc.Width || f.motionHeight > motionDesc.Height))
            throw std::runtime_error("Display motion extent exceeds its allocation");
        if (resampleMotion && motionDesc.Format != DXGI_FORMAT_R16G16_FLOAT &&
            motionDesc.Format != DXGI_FORMAT_R32G32_FLOAT && motionDesc.Format != DXGI_FORMAT_R16G16_SNORM &&
            motionDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && motionDesc.Format != DXGI_FORMAT_R32G32B32A32_FLOAT)
            throw std::runtime_error("Unsupported display motion format: " + Layout(f.motion));
        ID3D12Resource* exposureSource = nullptr;
        if (f.exposure)
        {
            const auto ed = f.exposure->GetDesc();
            if (ed.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && ed.SampleDesc.Count == 1 &&
                ed.DepthOrArraySize == 1 && !(ed.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
                (ed.Format == DXGI_FORMAT_R32_FLOAT || ed.Format == DXGI_FORMAT_R32G32_FLOAT ||
                 ed.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || ed.Format == DXGI_FORMAT_R16_FLOAT ||
                 ed.Format == DXGI_FORMAT_R16G16B16A16_FLOAT))
                exposureSource = f.exposure;
        }
        if (f.motion->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            throw std::runtime_error("Unsupported depth-stencil motion buffer: " + Layout(f.motion));
        p->activePasses = std::clamp(cfg.passes, 1u, 3u);
        bool passChange = p->lastPasses != p->activePasses;
        p->lastPasses = p->activePasses;
        for (UINT i = 0; i < p->activePasses; ++i)
            p->InitPass(i);
        p->InitShader();
        bool resize = p->width != w || p->height != h;
        if (resize)
        {
            p->colour.Reset();
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = w;
            rd.Height = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&p->colour)),
                  "Active FP16 texture");
            p->width = w;
            p->height = h;
        }
        auto motion = f.motion;
        auto depth = f.depth;
        auto createScratch = [&](ComPtr<ID3D12Resource>& resource, UINT sw, UINT sh, DXGI_FORMAT format)
        {
            if (resource && resource->GetDesc().Width == sw && resource->GetDesc().Height == sh)
                return;
            resource.Reset();
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = sw;
            rd.Height = sh;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = format;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&resource)),
                  "Guide scratch");
        };
        if (scaled)
        {
            createScratch(p->scaleBaseline, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            createScratch(p->scaleOutput, inputW, inputH, DXGI_FORMAT_R16G16B16A16_FLOAT);
            createScratch(p->depthCrop, w, h, DXGI_FORMAT_R32_FLOAT);
            depth = p->depthCrop.Get();
        }
        if (resampleMotion)
        {
            createScratch(p->motionCrop, w, h, DXGI_FORMAT_R16G16_FLOAT);
            motion = p->motionCrop.Get();
        }
        if (exposureSource)
            createScratch(p->exposureCopy, 1, 1, DXGI_FORMAT_R32_FLOAT);

        const bool guideChange = p->lastInputWidth != inputW || p->lastInputHeight != inputH ||
                                 p->lastMotionWidth != f.motionWidth || p->lastMotionHeight != f.motionHeight ||
                                 p->hadExposure != (exposureSource != nullptr);
        if (resize || guideChange || p->frames == 0)
            p->Log("Guide mapping: motion=" + std::to_string(f.motionWidth) + "x" + std::to_string(f.motionHeight) +
                   " resampled=" + std::to_string(resampleMotion) +
                   " exposure=" + (exposureSource ? Layout(exposureSource) : "auto") +
                   " preExposure=" + std::to_string(f.preExposure) + " tone=" + std::to_string(cfg.tone));
        auto cpu = p->heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = ReadFormat(f.colour->GetDesc().Format);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        p->device->CreateShaderResourceView(f.colour, &srv, cpu);
        cpu.ptr += p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p->device->CreateUnorderedAccessView(p->colour.Get(), nullptr, &uav, cpu);
        auto guideDescriptors = [&](UINT slot, ID3D12Resource* source, ID3D12Resource* target, DXGI_FORMAT format)
        {
            auto handle = p->heap->GetCPUDescriptorHandleForHeapStart();
            auto stride = p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            handle.ptr += slot * stride;
            auto guideSrv = srv;
            guideSrv.Format = ReadFormat(source->GetDesc().Format);
            p->device->CreateShaderResourceView(source, &guideSrv, handle);
            handle.ptr += stride;
            auto guideUav = uav;
            guideUav.Format = format;
            p->device->CreateUnorderedAccessView(target, nullptr, &guideUav, handle);
        };
        if (resampleMotion)
            guideDescriptors(4, f.motion, motion, DXGI_FORMAT_R16G16_FLOAT);
        if (exposureSource)
            guideDescriptors(6, exposureSource, p->exposureCopy.Get(), DXGI_FORMAT_R32_FLOAT);
        if (convertDepth)
        {
            // Distinct descriptor slots: overwriting the colour descriptors here
            // would change the earlier dispatch when the GPU consumes the list.
            cpu.ptr += p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            srv.Format = DepthReadFormat(depthDesc.Format);
            p->device->CreateShaderResourceView(f.depth, &srv, cpu);
            cpu.ptr += p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            uav.Format = DXGI_FORMAT_R32_FLOAT;
            p->device->CreateUnorderedAccessView(depth, nullptr, &uav, cpu);
        }
        p->diagnostics.Begin(cmd, p->frames + 1, f.sourceCall, p->activePasses, inputW, inputH, w, h);
        Barrier(cmd, f.colour, f.colourState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, p->colour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(p->root.Get());
        cmd->SetPipelineState(p->pipeline.Get());
        auto heap = p->heap.Get();
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        UINT dims[] { w, h, inputW, inputH };
        cmd->SetComputeRoot32BitConstants(1, 4, dims, 0);
        cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Barrier(cmd, p->colour.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, f.colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.colourState);
        Barrier(cmd, f.motion, f.motionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, f.depth, f.depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, exposureSource, f.exposureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto copyGuide = [&](ID3D12Resource* source, ID3D12Resource* dest)
        {
            if (source == dest)
                return;
            Barrier(cmd, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmd, dest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION from {}, to {};
            from.pResource = source;
            to.pResource = dest;
            D3D12_BOX box { 0, 0, 0, w, h, 1 };
            cmd->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            Barrier(cmd, dest, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        };
        if (resampleMotion)
        {
            Barrier(cmd, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(p->motionPipeline.Get());
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += 4 * p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cmd->SetComputeRootDescriptorTable(0, table);
            UINT motionDims[] { w, h, mvW, mvH };
            cmd->SetComputeRoot32BitConstants(1, 4, motionDims, 0);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            Barrier(cmd, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (convertDepth)
        {
            Barrier(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(p->depthPipeline.Get());
            cmd->SetComputeRoot32BitConstants(1, 4, dims, 0);
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += 2 * p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cmd->SetComputeRootDescriptorTable(0, table);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            Barrier(cmd, depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        else
            copyGuide(f.depth, depth);
        if (exposureSource)
        {
            auto exposure = p->exposureCopy.Get();
            Barrier(cmd, exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(p->exposurePipeline.Get());
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += 6 * p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cmd->SetComputeRootDescriptorTable(0, table);
            struct
            {
                UINT w, h;
                float preExposure, exposureScale;
            } constants { 1, 1, std::isfinite(f.preExposure) && f.preExposure > 0 ? f.preExposure : 1,
                          std::isfinite(f.exposureScale) && f.exposureScale > 0 ? f.exposureScale : 1 };
            cmd->SetComputeRoot32BitConstants(1, 4, &constants, 0);
            cmd->Dispatch(1, 1, 1);
            Barrier(cmd, exposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        UINT accepted = 0;
        if (scaled)
            copyGuide(p->colour.Get(), p->scaleBaseline.Get());
        const bool settingsChanged = cfg.modelScale != p->lastSettings.modelScale || !p->haveSettings ||
                                     cfg.tone != p->lastSettings.tone || cfg.structure != p->lastSettings.structure ||
                                     cfg.skin != p->lastSettings.skin;
        const UINT64 requestedGeneration = p->resetGeneration.load();
        const bool explicitReset = requestedGeneration != p->consumedResetGeneration;
        const unsigned continuityReasons = p->continuity.Pending();
        const bool gameReset = f.reset || (continuityReasons & Continuity::Game);
        const bool gap = (continuityReasons & Continuity::Gap) != 0;
        const bool bypass = (continuityReasons & Continuity::Bypass) != 0;
        // A busy async path can bypass on many consecutive calls. Preserve every
        // decision in buffered CSV without reopening the text log every frame.
        if (gameReset || (bypass && (p->frames < 3 || p->frames % 120 == 0)) ||
            resize || guideChange || passChange || p->resetAfterTimeout || settingsChanged ||
            explicitReset || gap)
            p->Log("AMD history reset: frame=" + std::to_string(p->frames) + " game=" + std::to_string(gameReset) +
                   " resize=" + std::to_string(resize) + " guides=" + std::to_string(guideChange) +
                   " passes=" + std::to_string(passChange) + " timeout=" + std::to_string(p->resetAfterTimeout) +
                   " settings=" + std::to_string(settingsChanged) + " explicit=" + std::to_string(explicitReset) +
                   " gap=" + std::to_string(gap) + " bypass=" + std::to_string(bypass) +
                   " source_call=" + std::to_string(f.sourceCall));
        for (UINT i = 0; i < p->activePasses; ++i)
        {
            auto r = p->runtime[i];
            At<uint8_t>(r, 0x76e1d) = 1;
            // Engine +0x120 is the history-valid flag, +0x118 is the current
            // borrowed history view. Clear only at a quiescent frame boundary.
            if (gameReset || bypass || resize || guideChange || passChange || p->resetAfterTimeout || settingsChanged ||
                explicitReset || gap)
            {
                At<uint8_t>(r, 0x765f8) = 0;
                At<void*>(r, 0x765f0) = nullptr;
            }
            At<UINT>(r, 0x76e10) = f.depthInverted;
            At<uint8_t>(r, 0x76e14) = 1; // explicit depth convention, no heuristic
            At<float>(r, 0x76e30) = i == 0 ? cfg.tone : 0;
            At<float>(r, 0x76e34) = cfg.structure;
            At<float>(r, 0x76e38) = cfg.skin;
            Packet packet {};
            packet.list = cmd;
            packet.colour = p->colour.Get();
            packet.colourState = 4;
            packet.motion = motion;
            packet.motionState = 4;
            packet.depth = depth;
            packet.depthState = 4;
            packet.exposure = exposureSource ? p->exposureCopy.Get() : nullptr;
            packet.exposureState = 4;
            packet.scaleX = f.motionScaleX * (resampleMotion ? float(w) / mvW : 1.0f);
            packet.scaleY = f.motionScaleY * (resampleMotion ? float(h) / mvH : 1.0f);
            p->diagnostics.Mark(cmd, 2 * i + 1);
            if (split)
                split->Arm(&At<ID3D12RootSignature*>(r, 0x76c00), &At<ID3D12PipelineState*>(r, 0x76c08),
                           &At<ID3D12Resource*>(r, 0x76be8));
            reinterpret_cast<RecordFn>(reinterpret_cast<uintptr_t>(r) + 0xa0b0)(&packet);
            if (split && !split->EndNative())
            {
                p->failed = true;
                p->Log("Phased record rejected: native capture/wait boundary did not match; no segments submitted");
            }
            p->diagnostics.Mark(cmd, 2 * i + 2);
            p->jobs[i] = At<UINT>(r, 0x76d74);
            p->workerTailPolls[i] = 0;
            // A staging resize resets the counter before this job is published.
            p->observedTimeouts[i] = At<UINT>(r, 0x76c18);
            // Staging recreation resets the native job counter. After a resize,
            // job 1 can follow job 1, so counter equality does not mean rejection.
            // The native pending-list pointer is the actual submission contract.
            const bool recorded = At<ID3D12CommandList*>(r, 0x76d68) == cmd;
            if (recorded)
            {
                // Recreated staging restarts job IDs, but the mapped watchdog
                // abort word can retain a larger ID from the preceding extent.
                // At this point the preceding GPU fence and HIP job have retired,
                // and this list has not been submitted. Clear the obsolete abort
                // token; the watchdog remains active once Notify publishes this job.
                if (auto abortWord = At<volatile LONG*>(r, 0x76c68))
                    InterlockedExchange(abortWord, 0);
                ++accepted;
            }
            if (At<uint8_t>(r, 0x767fa) || !recorded)
            {
                p->failed = true;
                p->Log("AMD pass rejected frame: " + std::to_string(i + 1) + " job=" + std::to_string(p->jobs[i]) +
                       " pending=" + std::to_string(recorded) +
                       " nativeFailure=" + std::to_string(At<uint8_t>(r, 0x767fa)));
                // Even on failure, any recorded work must be published after
                // submission so its GPU-side wait is not left without a worker.
                break;
            }
        }
        Barrier(cmd, f.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.motionState);
        Barrier(cmd, f.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.depthState);
        Barrier(cmd, exposureSource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.exposureState);
        p->activePasses = accepted;
        if (accepted)
        {
            ++p->frames;
            p->retainedList = cmd;
            p->retainedInputs = { incoming.colour, incoming.motion, incoming.depth, incoming.exposure };
            p->firstPublished = false;
            p->pending.store(cmd, std::memory_order_release);
        }
        if (p->failed)
            return nullptr;

        auto finalColour = p->colour.Get();
        if (scaled)
        {
            guideDescriptors(10, f.colour, p->scaleOutput.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            auto handle = p->heap->GetCPUDescriptorHandleForHeapStart();
            auto stride = p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            handle.ptr += 12 * stride;
            auto v = srv;
            v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            p->device->CreateShaderResourceView(p->scaleBaseline.Get(), &v, handle);
            handle.ptr += stride;
            p->device->CreateShaderResourceView(finalColour, &v, handle);
            Barrier(cmd, f.colour, f.colourState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, p->scaleOutput.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(p->root.Get());
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetPipelineState(p->resolvePipeline.Get());
            auto table = heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += 10 * stride;
            cmd->SetComputeRootDescriptorTable(0, table);
            table.ptr += 2 * stride;
            cmd->SetComputeRootDescriptorTable(2, table);
            UINT rc[] { inputW, inputH, w, h };
            cmd->SetComputeRoot32BitConstants(1, 4, rc, 0);
            cmd->Dispatch((inputW + 7) / 8, (inputH + 7) / 8, 1);
            Barrier(cmd, p->scaleOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, f.colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.colourState);
            finalColour = p->scaleOutput.Get();
        }
        if (!accepted)
            return nullptr;
        p->diagnostics.End(cmd);
        p->activeSourceCall = f.sourceCall;
        p->consumedReasons = continuityReasons | (explicitReset ? 8u : 0u) | (settingsChanged ? 16u : 0u) |
                             (resize ? 32u : 0u) | (guideChange ? 64u : 0u) |
                             (p->resetAfterTimeout ? 128u : 0u) | (passChange ? 256u : 0u);
        p->resetConsumedCall = f.sourceCall;
        p->continuity.Accepted(continuityReasons);
        // A concurrent invalidation after our snapshot remains pending.
        p->consumedResetGeneration = requestedGeneration;
        p->resetAfterTimeout = false;
        p->lastRecordReason = "recorded";
        p->lastSettings = cfg;
        p->haveSettings = true;
        p->lastInputWidth = inputW;
        p->lastInputHeight = inputH;
        p->lastMotionWidth = f.motionWidth;
        p->lastMotionHeight = f.motionHeight;
        p->hadExposure = exposureSource != nullptr;
        if (p->frames <= 3 || resize)
            p->Log("Recorded pre-SR " + std::to_string(w) + "x" + std::to_string(h) +
                   " passes=" + std::to_string(p->activePasses));
        return finalColour;
    }
    catch (const std::exception& e)
    {
        p->failed = true;
        p->Log(e.what());
        return nullptr;
    }
}
ID3D12Resource* Backend::RecordPhased(ID3D12GraphicsCommandList* outer, ID3D12CommandQueue* queue,
                                    const Frame& incoming, const Settings& settings)
{
    std::lock_guard phaseGuard(p->phasedLock);
    if (!outer || !queue || !Ready()) return nullptr;
    // This first integration is synchronous and is admitted only by the prepared
    // D3D11 bridge. Other producers may still be recording inputs into outer.
    if (p->perf.asyncSingle || p->perf.diagnosticStages || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        p->Log("Phased NR requires a direct queue, AsyncSinglePass=0 and DiagnosticStages=0");
        return nullptr;
    }
    if (incoming.colourState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ||
        incoming.motionState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ||
        incoming.depthState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ||
        (incoming.exposure && incoming.exposureState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
    {
        p->Log("Phased NR requires prepared shader-readable input states; NR bypassed");
        return nullptr;
    }
    const double started = Perf::NowMs();
    bool submittedAny = false;
    try
    {
        if (!p->phases)
        {
            p->phases.Attach(new SplitCommandList());
            Check(p->phases->Init(p->device.Get()), "Phased command resources");
            p->Log("Phased submission active: capture fence -> native worker retirement -> apply -> outer consumer fence");
        }
        Check(p->phases->Begin(), "Phased allocator reset");
        auto result = RecordImpl(p->phases.Get(), incoming, settings, p->phases.Get());
        const auto close = p->phases->Close();
        if (!result)
        {
            std::lock_guard guard(p->lock);
            p->CancelUnsubmittedPhased();
            return nullptr;
        }
        Check(close, "Phased command close/boundary validation");
        if (p->phases->SegmentCount() != p->activePasses + 1)
            throw std::runtime_error("Phased native segment count mismatch; nothing submitted");
        ID3D12CommandList* nativeList = p->phases.Get();
        Submitting(queue, 1, &nativeList); // Bind the same queue for every native pass.
        std::lock_guard guard(p->lock);
        auto executeAndWait = [&](UINT segment) -> UINT64
        {
            auto list = p->phases->Segment(segment);
            submittedAny = true;
            queue->ExecuteCommandLists(1, &list);
            const UINT64 target = ++p->phases->serial;
            Check(queue->Signal(p->phases->fence.Get(), target), "Phased segment Signal");
            Check(Perf::WaitForInput(p->phases->fence.Get(), target, p->device.Get()), "Phased segment completion");
            return target;
        };
        p->lastSubmitted = GetTickCount64();
        for (UINT i=0; i<p->activePasses; ++i)
        {
            // Segment i contains capture i, and for i>0 the apply of the
            // preceding pass whose HIP worker has already retired. It contains
            // NO wait on a worker that has yet to be notified.
            const double captureStart = Perf::NowMs();
            const auto target = executeAndWait(i);
            const double captureMs = Perf::NowMs() - captureStart;
            auto h = p->runtime[i];
            p->submittedMs = Perf::NowMs();
            const double notifyStart = Perf::NowMs();
            reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(h) + 0x4640)(queue, 1, &nativeList);
            p->notifyMs = Perf::NowMs() - notifyStart;
            p->firstPublished = true;
            UINT polls=0;double sleepMs=0,maxSleep=0;
            while (!p->WorkerRetired(i))
            {
                Check(p->device->GetDeviceRemovedReason(), "Phased device status");
                if (Perf::NowMs() - p->submittedMs > 5000)
                    throw std::runtime_error("Phased HIP worker exceeded 5 seconds; output not applied, resources retained");
                const double before = Perf::NowMs();
                Sleep(1);
                const double elapsed = Perf::NowMs() - before;
                ++polls;sleepMs+=elapsed;maxSleep=(std::max)(maxSleep,elapsed);
            }
            p->TimingRow(i, false, polls, sleepMs, maxSleep);
            const auto worker = WorkerCompletion::Read(h);
            if (p->perf.timing)
                p->phaseTiming.Row(GetTickCount64(), p->activeSourceCall, p->frames, i+1, "capture_worker",
                                   p->phases->SegmentCount(), captureMs, Perf::NowMs()-p->submittedMs,
                                   0, Perf::NowMs()-started, target, p->phases->fence->GetCompletedValue(),
                                   worker.inference, worker.retired);
        }
        const double applyStart = Perf::NowMs();
        const auto target = executeAndWait(p->activePasses);
        if (p->perf.timing)
            p->phaseTiming.Row(GetTickCount64(), p->activeSourceCall, p->frames, 0, "apply_complete",
                               p->phases->SegmentCount(), 0, 0, Perf::NowMs()-applyStart, Perf::NowMs()-started,
                               target, p->phases->fence->GetCompletedValue(), 0, 0);
        // Keep the final output and all allocators pinned until the caller's
        // upscaler has consumed it. Submitted handles this list without a second
        // native notification; the normal completion fence protects its lifetime.
        p->retainedList = outer;
        p->phasedAwaitOuter = true;
        p->pending.store(outer, std::memory_order_release);
        p->status = "Phased NR complete; waiting for outer upscaler submission";
        if (p->CollectTimeouts()) return nullptr;
        return result;
    }
    catch (const std::exception& e)
    {
        std::lock_guard guard(p->lock);
        p->failed = true;
        if (!submittedAny) p->CancelUnsubmittedPhased();
        p->phaseTiming.Flush();p->timing.Flush();
        p->Log(std::string("Phased NR stopped: ") + e.what());
        // Never retry/recreate buffers while an unretired segment or worker may
        // still access them. The prepared input itself was not overwritten.
        return nullptr;
    }
}
int Backend::PendingListIndex(UINT count, ID3D12CommandList* const* lists) const
{
    auto pending = p->pending.load(std::memory_order_acquire);
    if (!pending || !lists)
        return -1;
    for (UINT i = 0; i < count; ++i)
        if (lists[i] == pending)
            return static_cast<int>(i);
    return -1;
}
void Backend::Submitting(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists)
{
    auto pending = p->pending.load(std::memory_order_acquire);
    if (!pending || !queue)
        return;
    bool found = false;
    for (UINT i = 0; i < n; ++i)
        found |= lists[i] == pending;
    if (!found)
        return;
    std::lock_guard guard(p->lock);
    if (p->pending.load() != pending || p->firstPublished)
        return;
    if (p->frames <= 3)
        p->Log("Neural submission: lists=" + std::to_string(n) +
               " queueType=" + std::to_string(static_cast<UINT>(queue->GetDesc().Type)));
    // Match the recorded list, not the swapchain's presentation queue. FG can
    // replace the latter, and the renderer may also migrate between queues.
    // Only one frame is outstanding, so the prior completion fence has retired
    // before Record permits this frame to use the shared runtime resources.
    if (queue != p->queue.Get())
    {
        for (auto h : p->runtime)
            if (h)
            {
                auto old = At<ID3D12CommandQueue*>(h, 0x764d0);
                queue->AddRef();
                At<ID3D12CommandQueue*>(h, 0x764d0) = queue;
                if (old)
                    old->Release();
            }
        p->queue = queue;
        p->Log("Render submission queue changed; AMD pre-SR remains enabled");
    }
    // Bind the real queue here, but only wake HIP after ExecuteCommandLists.
    // A capture-wait kernel launched before D3D12 submission can occupy the GPU
    // while the capture it depends on is still queued on the CPU.
}
void Backend::Submitted(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists)
{
    // Fallback for callers using the original post-submit API.
    Submitting(queue, n, lists);
    auto pending = p->pending.load(std::memory_order_acquire);
    if (!pending || !queue)
        return;
    bool found = false;
    for (UINT i = 0; i < n; ++i)
        found |= lists[i] == pending;
    if (!found)
        return;
    std::lock_guard guard(p->lock);
    if (p->pending.load() != pending)
        return;
    if (p->phasedAwaitOuter)
    {
        // Phased NR is complete, but the outer upscaler still consumes its
        // output. Retain all resources until THIS actual queue submission ends.
        const auto value = ++p->serial;
        if (FAILED(queue->Signal(p->fence.Get(), value)))
        {
            p->failed = true;
            p->Log("Phased outer consumer Signal failed; resources retained");
            return;
        }
        p->completion.store(value);
        p->phasedAwaitOuter = false;
        p->pending.store(nullptr, std::memory_order_release);
        ++p->completedFrames;
        p->lastCompleted = GetTickCount64();
        p->status = "Completed phased AMD pre-SR passes=" + std::to_string(p->activePasses) + " at " +
                    std::to_string(p->width) + "x" + std::to_string(p->height);
        if (p->completedFrames <= 3 || p->completedFrames % 120 == 0) p->Log(p->status);
        return;
    }
    if (p->asyncSingle)
        return;
    p->diagnostics.Submitted(queue);
    p->lastSubmitted = GetTickCount64();
    if (p->activePasses == 1 && p->perf.asyncSingle)
    {
        auto h = p->runtime[0];
        p->submittedMs = Perf::NowMs();
        p->Stage(0, "before_notify", true);
        const auto notifyStart = Perf::NowMs();
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(h) + 0x4640)(queue, n, lists);
        p->notifyMs = Perf::NowMs() - notifyStart;
        p->Stage(0, "after_notify", true);
        p->firstPublished = true;
        auto value = ++p->serial;
        if (FAILED(queue->Signal(p->fence.Get(), value)))
        {
            p->failed = true;
            p->Log("D3D12 completion Signal failed; resources retained");
            return;
        }
        p->completion.store(value);
        p->asyncStart = GetTickCount64();
        p->asyncSingle = true;
        return;
    }
    for (UINT i = 0; i < p->activePasses; ++i)
    {
        auto h = p->runtime[i];
        p->submittedMs = Perf::NowMs();
        p->Stage(i, "before_notify", true);
        const auto notifyStart = Perf::NowMs();
        if (i != 0 || !p->firstPublished)
            reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(h) + 0x4640)(queue, n, lists);
        p->notifyMs = Perf::NowMs() - notifyStart;
        p->Stage(i, "after_notify", true);
        UINT polls = 0;
        double sleepMs = 0, maxSleep = 0;
        // All runtimes use HIP stream 0. Publish the next pass only once the previous
        // worker finished; otherwise its capture-wait kernel could block the first pass.
        auto start = GetTickCount64();
        // Early inference completion is not worker retirement: it is published
        // before flag readback and timeout accounting. Do not permit the next
        // pass/frame, staging recreation, or teardown to race that tail.
        while (!p->WorkerRetired(i))
        {
            if (GetTickCount64() - start > 5000)
            {
                p->failed = true;
                p->Log("HIP completion timeout pass " + std::to_string(i + 1));
                break;
            }
            if (FAILED(p->device->GetDeviceRemovedReason()))
            {
                p->failed = true;
                p->Log("AMD stopped: device removed during completion wait");
                break;
            }
            p->Stage(i, "waiting");
            const auto beforeSleep = Perf::NowMs();
            Sleep(1);
            const auto elapsed = Perf::NowMs() - beforeSleep;
            ++polls;
            sleepMs += elapsed;
            maxSleep = (std::max)(maxSleep, elapsed);
        }
        p->Stage(i, "native_wait_end", true);
        p->TimingRow(i, false, polls, sleepMs, maxSleep);
        if (p->failed)
            break;
    }
    UINT64 value = ++p->serial;
    if (FAILED(queue->Signal(p->fence.Get(), value)))
    {
        p->failed = true;
        p->Log("D3D12 completion Signal failed");
    }
    p->completion.store(value);
    p->pending.store(nullptr, std::memory_order_release);
    const bool completionTimedOut = p->CollectTimeouts();
    if (!p->failed)
    {
        if (completionTimedOut)
        {
            // Record consumes the native counters and schedules safe recovery.
            // A completed HIP job is not proof that its GPU output was applied.
            p->Log("AMD timeout: current input preserved; recovery pending");
            return;
        }
        ++p->completedFrames;
        p->lastCompleted = GetTickCount64();
        auto completed = "Completed AMD pre-SR passes=" + std::to_string(p->activePasses) + " at " +
                         std::to_string(p->width) + "x" + std::to_string(p->height);
        if (p->frames <= 3 || p->frames % 120 == 0)
            p->Log(completed);
        else
            p->status = completed;
    }
}
std::string Backend::Status() const
{
    std::lock_guard guard(p->lock);
    p->RetireSingle();
    auto reportedTimeouts = p->timeoutEvents;
    for (UINT i = 0; i < p->runtime.size(); ++i)
        if (p->runtime[i])
        {
            auto count = At<UINT>(p->runtime[i], 0x76c18);
            if (count > p->observedTimeouts[i])
                reportedTimeouts += count - p->observedTimeouts[i];
        }
    if (!p->failed && p->lastSubmitted)
        return p->status + " | completed frames=" + std::to_string(p->completedFrames) +
               (p->lastCompleted
                    ? " last completion " + std::to_string((GetTickCount64() - p->lastCompleted) / 1000) + "s ago"
                    : " no successful completion") +
               " | timeout events=" + std::to_string(reportedTimeouts) +
               " | bridge recorded/bypassed=" + std::to_string(p->bridgeRecorded) + "/" +
                   std::to_string(p->bridgeBypassed) +
               " | skipped pending/GPU=" + std::to_string(p->pendingSkips) + "/" + std::to_string(p->fenceSkips);
    return p->status;
}
UINT64 Backend::RecordedFrames() const { return p->frames; }
void Backend::InvalidateHistory() { p->resetGeneration.fetch_add(1); }
UINT64 Backend::BeginFrame(bool gameReset)
{
    std::lock_guard guard(p->lock);
    return p->continuity.Observe(GetTickCount64(), gameReset);
}
void Backend::TraceFrame(UINT64 sourceCall, const char* outcome)
{
    std::lock_guard guard(p->lock);
    if (std::strcmp(outcome, "record_rejected") == 0)
        outcome = p->lastRecordReason;
    if (std::strcmp(outcome, "recorded") == 0)
        ++p->bridgeRecorded;
    else
    {
        ++p->bridgeBypassed;
        p->continuity.Skipped();
    }
    if (!p->perf.timing)
        return;
    auto h = p->runtime[0];
    auto count = [&](size_t offset) -> UINT {
        return h ? static_cast<UINT>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&At<UINT>(h, offset)), 0, 0)) : 0;
    };
    p->decisions.Row(GetTickCount64(), sourceCall, outcome, p->frames, p->jobs[0], p->continuity.Pending(),
                     p->resetConsumedCall == sourceCall ? p->consumedReasons : 0,
                     p->resetGeneration.load(), p->consumedResetGeneration, count(0x76c14), count(0x76c18),
                     p->pending.load() != nullptr, p->fence ? p->fence->GetCompletedValue() : 0,
                     p->completion.load(), p->lastInputWidth, p->lastInputHeight, p->width, p->height, count(0x76d60));
}
bool Backend::Ready()
{
    std::lock_guard guard(p->lock);
    p->RetireSingle();
    const bool ready = !p->failed && !p->pending.load() && p->fence->GetCompletedValue() >= p->completion.load() && p->WorkersRetired();
    if (ready) p->FinishDiagnostics();
    return ready;
}
bool Backend::Shutdown()
{
    std::lock_guard guard(p->lock);
    p->RetireSingle();
    if (p->pending.load() || p->fence->GetCompletedValue() < p->completion.load() || !p->WorkersRetired())
        return false;
    p->FinishDiagnostics();
    for (auto h : p->runtime)
        if (h)
        {
            if (p->hipSet)
                p->hipSet(p->hipDevice);
            reinterpret_cast<void (*)()>(reinterpret_cast<uintptr_t>(h) + 0xc520)();
        }
    p->ReleaseInputs();
    p->timing.Flush();
    p->timerResolution.Stop();
    p->failed = true;
    p->timing.Flush();
    p->diagnostics.Flush();
    p->decisions.Flush();
    p->phaseTiming.Flush();
    p->Log("Workers stopped outside loader lock");
    return true;
}
} // namespace AmdPreSr
