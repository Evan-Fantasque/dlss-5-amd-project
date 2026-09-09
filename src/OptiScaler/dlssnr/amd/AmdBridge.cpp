#include "pch.h"
#include "AmdBridge.h"
#include "AmdPreSr.h"
#include "Performance.h"
#include "UpscaleGate.h"
#include <State.h>
#include <Util.h>
#include <detours/detours.h>
#include <atomic>
#include <mutex>
#include <cmath>
#include <algorithm>

namespace DlssNr::AmdBridge
{
namespace
{
std::atomic<AmdPreSr::Backend*> backend { nullptr };
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ExitFn = void(NTAPI*)(LONG);
ExecuteFn executeOriginal = nullptr;
ExitFn exitOriginal = nullptr;
std::string message = "AMD pre-SR: waiting for a DirectX 12 SR frame";
std::mutex messageMutex;
std::mutex initMutex;
std::mutex frameMutex;
void Message(const char* s)
{
    std::lock_guard l(messageMutex);
    message = s;
}
thread_local NVSDK_NGX_Parameter* replacedParams = nullptr;
thread_local ID3D12Resource* originalColour = nullptr;
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* c)
{
    if (auto b = backend.load())
        b->Submitting(q, n, c);
    executeOriginal(q, n, c);
    if (auto b = backend.load())
        b->Submitted(q, n, c);
}
void NTAPI Exit(LONG code)
{
    if (auto b = backend.load())
        b->Shutdown();
    exitOriginal(code);
}
std::filesystem::path Directory() { return Util::DllPath().parent_path(); }
ID3D12Resource* Resource(NVSDK_NGX_Parameter* p, const char* name)
{
    ID3D12Resource* r = nullptr;
    if (p->Get(name, &r) != NVSDK_NGX_Result_Success)
        p->Get(name, reinterpret_cast<void**>(&r));
    return r;
}
bool IsAmd(ID3D12Device* d)
{
    // Query the real SR adapter, not the identity spoofed for the game's DLSS
    // availability checks. This guard affects only this thread and scope.
    ScopedSkipSpoofingThread skipSpoofingThread {};
    IDXGIFactory4* f = nullptr;
    IDXGIAdapter1* a = nullptr;
    bool amd = false;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
    {
        if (SUCCEEDED(f->EnumAdapterByLuid(d->GetAdapterLuid(), IID_PPV_ARGS(&a))))
        {
            DXGI_ADAPTER_DESC1 desc {};
            const auto result = a->GetDesc1(&desc);
            amd = SUCCEEDED(result) && desc.VendorId == 0x1002;
            LOG_INFO("AMD pre-SR adapter identity: vendor=0x{:04X}, AMD={}, result=0x{:08X}", desc.VendorId, amd,
                     static_cast<UINT>(result));
            a->Release();
        }
        f->Release();
    }
    return amd;
}
} // namespace
bool HasFiles()
{
    // A proxy can load before its final path is known. Cache only success.
    static std::atomic<bool> present { false };
    if (present.load(std::memory_order_relaxed))
        return true;
    std::error_code ec;
    if (std::filesystem::exists(Directory() / L"dlssnr_amd_pass1.dll", ec))
        present.store(true, std::memory_order_relaxed);
    return present.load(std::memory_order_relaxed);
}
void ObserveConfiguration()
{
    if (!HasFiles())
        return;
    // Retire completed work even when NR has just been switched off.
    if (auto b = backend.load())
        b->Ready();
    static std::mutex observationMutex;
    std::lock_guard guard(observationMutex);
    static bool haveState = false, previousNr = false, previousFg = false, previousPre = false;
    static float previousScale = 0;
    static UINT previousPasses = 0;
    const auto& cfg = *Config::Instance();
    const bool nr = cfg.DlssNrEnabled.value_or_default(), fg = cfg.FGEnabled.value_or_default();
    const bool pre = cfg.DlssNrRunBeforeSr.value_or_default();
    const float scale = cfg.AmdNrScale.value_or_default();
    const UINT passes = cfg.DlssNrPasses.value_or_default();
    if (!haveState || nr != previousNr || fg != previousFg || pre != previousPre || scale != previousScale ||
        passes != previousPasses)
    {
        LOG_INFO("AMD test8 state: tick_ms={} NR={} FG_config={} preSR={} NR_scale={} passes={}", GetTickCount64(), nr,
                 fg, pre, scale, passes);
        // Configuration is not proof that FG dispatch succeeded; label it explicitly.
        if (!haveState || nr != previousNr || fg != previousFg)
            InvalidateHistory();
        haveState = true;
        previousNr = nr;
        previousFg = fg;
        previousPre = pre;
        previousScale = scale;
        previousPasses = passes;
    }
}
bool Before(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params, ID3D12CommandQueue* q)
{
    std::lock_guard frameGuard(frameMutex);
    if (!HasFiles())
        return false;
    ID3D12Device* device = nullptr;
    if (!cmd || !params || FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
        return true;
    thread_local LUID checkedAdapter {};
    thread_local bool checked = false, amd = false;
    const auto adapter = device->GetAdapterLuid();
    if (!checked || adapter.HighPart != checkedAdapter.HighPart || adapter.LowPart != checkedAdapter.LowPart)
    {
        amd = IsAmd(device);
        checkedAdapter = adapter;
        checked = true;
    }
    if (!amd)
    {
        Message("AMD pre-SR: SR adapter not identified as AMD; see OptiScaler.log");
        device->Release();
        return false;
    }
    // Check actual active input before creating the HIP backend or loading weights.
    // Feature-create Width is a requested size and can already be reduced in menus.
    // Only Render_Subrect reflects this call; texture dimensions are a fallback.
    static const auto gateOptions = AmdPreSr::Perf::Options::Read(Directory());
    UINT activeWidth = 0, activeHeight = 0, outputWidth = 0, outputHeight = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &activeWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &activeHeight);
    if (auto colour = Resource(params, NVSDK_NGX_Parameter_Color))
    {
        const auto desc = colour->GetDesc();
        if (!activeWidth) activeWidth = static_cast<UINT>(desc.Width);
        if (!activeHeight) activeHeight = desc.Height;
        if (activeWidth > desc.Width || activeHeight > desc.Height)
            activeWidth = activeHeight = 0;
    }
    else
        activeWidth = activeHeight = 0;
    params->Get(NVSDK_NGX_Parameter_OutWidth, &outputWidth);
    params->Get(NVSDK_NGX_Parameter_OutHeight, &outputHeight);
    if (auto output = Resource(params, NVSDK_NGX_Parameter_Output))
    {
        const auto desc = output->GetDesc();
        if (!outputWidth) outputWidth = static_cast<UINT>(desc.Width);
        if (!outputHeight) outputHeight = desc.Height;
        if (outputWidth > desc.Width || outputHeight > desc.Height)
            outputWidth = outputHeight = 0;
    }
    else
        outputWidth = outputHeight = 0;
    static AmdPreSr::UpscaleGate gate;
    const auto admission = gate.Observe(gateOptions.requireUpscale, activeWidth, activeHeight,
                                        outputWidth, outputHeight);
    if (admission.changed)
    {
        LOG_INFO("AMD test8 SR gate: tick_ms={} state={} required={} input={}x{} output={}x{} blocked_calls={} backend_created={}",
                 GetTickCount64(), admission.reason, gateOptions.requireUpscale, activeWidth, activeHeight,
                 outputWidth, outputHeight, gate.BlockedCalls(), backend.load() != nullptr);
    }
    if (!admission.allowed)
    {
        // Already-submitted work retains its resources. Never reset GPU history
        // here; preserve any one-frame Reset until the next admitted job instead.
        if (auto existing = backend.load())
        {
            existing->Ready();
            UINT reset = 0;
            params->Get(NVSDK_NGX_Parameter_Reset, &reset);
            const auto call = existing->BeginFrame(reset != 0);
            AmdPreSr::Perf::bridgeSourceCall = call;
            existing->TraceFrame(call, admission.reason);
        }
        Message("AMD pre-SR: paused (RequireUpscale=1; waiting for reduced SR input)");
        device->Release();
        // Handled by AMD: bypass NR only, and never fall through to NVIDIA NR.
        return true;
    }
    if (!q)
        q = reinterpret_cast<ID3D12CommandQueue*>(State::Instance().currentCommandQueue);
    if (!q)
    {
        device->Release();
        Message("AMD pre-SR: waiting for the game command queue");
        return true;
    }
    std::lock_guard initGuard(initMutex);
    auto b = backend.load();
    if (!b)
    {
        executeOriginal = reinterpret_cast<ExecuteFn>((*reinterpret_cast<void***>(q))[10]);
        // FG can expose a proxy present queue. Hook the device's execution
        // implementation so actual render submissions are still observed.
        ID3D12CommandQueue* probe = nullptr;
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        if (SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&probe))))
        {
            executeOriginal = reinterpret_cast<ExecuteFn>((*reinterpret_cast<void***>(probe))[10]);
            probe->Release();
        }
        exitOriginal = reinterpret_cast<ExitFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserProcess"));
        LONG err = DetourTransactionBegin();
        if (err == NO_ERROR)
            err = DetourUpdateThread(GetCurrentThread());
        if (err == NO_ERROR)
            err = DetourAttach(reinterpret_cast<PVOID*>(&executeOriginal), Execute);
        if (err == NO_ERROR && exitOriginal)
            err = DetourAttach(reinterpret_cast<PVOID*>(&exitOriginal), Exit);
        if (err == NO_ERROR)
            err = DetourTransactionCommit();
        else
            DetourTransactionAbort();
        if (err != NO_ERROR)
        {
            device->Release();
            Message("AMD pre-SR: could not install submission notification");
            return true;
        }
        b = new AmdPreSr::Backend(device, q, Directory());
        backend.store(b);
    }
    device->Release();
    Message("");
    // The swapchain's present queue can change when FG is enabled. It is
    // only a bootstrap hint; Submitted identifies the queue executing our list.
    UINT reset = 0;
    params->Get(NVSDK_NGX_Parameter_Reset, &reset);
    const UINT64 call = b->BeginFrame(reset != 0);
    AmdPreSr::Perf::bridgeSourceCall = call;
    struct Decision
    {
        AmdPreSr::Backend* backend;
        UINT64 call;
        const char* outcome = "invalid_input";
        ~Decision() { backend->TraceFrame(call, outcome); }
    } decision { b, call };
    AmdPreSr::Frame f {};
    f.sourceCall = call;
    f.reset = reset != 0;
    f.colour = Resource(params, NVSDK_NGX_Parameter_Color);
    f.motion = Resource(params, NVSDK_NGX_Parameter_MotionVectors);
    f.depth = Resource(params, NVSDK_NGX_Parameter_Depth);
    f.exposure = Resource(params, NVSDK_NGX_Parameter_ExposureTexture);
    params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &f.preExposure);
    params->Get(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &f.exposureScale);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &f.width);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &f.height);
    if (f.colour)
    {
        const auto extent = f.colour->GetDesc();
        if (!f.width)
            f.width = static_cast<UINT>(extent.Width);
        if (!f.height)
            f.height = extent.Height;
    }
    UINT x = 0, y = 0, flags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &x);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &y);
    if (x || y)
    {
        decision.outcome = "unsupported_origin";
        Message("AMD pre-SR: nonzero colour subrect origin unsupported");
        return true;
    }
    auto haveFlags = params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags) == NVSDK_NGX_Result_Success;
    if (haveFlags && !(flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) && f.motion)
    {
        params->Get(NVSDK_NGX_Parameter_OutWidth, &f.motionWidth);
        params->Get(NVSDK_NGX_Parameter_OutHeight, &f.motionHeight);
        if (!f.motionWidth)
            f.motionWidth = static_cast<UINT>(f.motion->GetDesc().Width);
        if (!f.motionHeight)
            f.motionHeight = f.motion->GetDesc().Height;
    }
    if (!b->Ready())
    {
        decision.outcome = "backend_not_ready";
        return true;
    }
    // Commit resource changes only after a short stable interval. Reuse no old frame.
    static UINT inputWidth = 0, inputHeight = 0;
    static float inputScale = 1;
    static ULONGLONG stableSince = 0;
    const float configuredScale = Config::Instance()->AmdNrScale.value_or_default();
    const float scale = std::isfinite(configuredScale) ? std::clamp(configuredScale, .25f, 1.f) : 1.f;
    const auto now = GetTickCount64();
    if (inputWidth != f.width || inputHeight != f.height || inputScale != scale)
    {
        inputWidth = f.width;
        inputHeight = f.height;
        inputScale = scale;
        stableSince = now;
        b->InvalidateHistory();
    }
    if (now - stableSince < 300)
    {
        decision.outcome = "resolution_settling";
        Message("AMD pre-SR: waiting for resolution settings to settle");
        return true;
    }
    f.depthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;

    params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &f.motionScaleX);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &f.motionScaleY);
    const auto& cfg = *Config::Instance();
    if (cfg.ColorResourceBarrier.has_value())
        f.colourState = static_cast<D3D12_RESOURCE_STATES>(cfg.ColorResourceBarrier.value());
    if (cfg.MVResourceBarrier.has_value())
        f.motionState = static_cast<D3D12_RESOURCE_STATES>(cfg.MVResourceBarrier.value());
    if (cfg.DepthResourceBarrier.has_value())
        f.depthState = static_cast<D3D12_RESOURCE_STATES>(cfg.DepthResourceBarrier.value());
    if (cfg.ExposureResourceBarrier.has_value())
        f.exposureState = static_cast<D3D12_RESOURCE_STATES>(cfg.ExposureResourceBarrier.value());
    AmdPreSr::Settings s {};
    s.modelScale = scale;
    s.passes = cfg.DlssNrPasses.value_or_default();
    s.tone = cfg.DlssNrLocalTone.value_or_default();
    s.structure = cfg.DlssNrLocalStructure.value_or_default();
    s.skin = cfg.DlssNrSkinStructure.value_or_default();
    if (s.skin < 0)
        s.skin = s.structure;
    decision.outcome = "record_rejected";
    auto replacement = gateOptions.phasedSubmission && AmdPreSr::Perf::preparedDx11Inputs
        ? b->RecordPhased(cmd, q, f, s) : b->Record(cmd, f, s);
    if (replacement)
    {
        decision.outcome = "recorded";
        originalColour = f.colour;
        replacedParams = params;
        params->Set(NVSDK_NGX_Parameter_Color, replacement);
    }
    return true;
}
void Restore(NVSDK_NGX_Parameter* params)
{
    if (params && replacedParams == params)
    {
        params->Set(NVSDK_NGX_Parameter_Color, originalColour);
        replacedParams = nullptr;
        originalColour = nullptr;
    }
}
void InvalidateHistory()
{
    if (auto b = backend.load())
        b->InvalidateHistory();
}
std::string Status()
{
    {
        std::lock_guard l(messageMutex);
        if (!message.empty())
            return message;
    }
    if (auto b = backend.load())
        return b->Status();
    return "AMD pre-SR: idle";
}
} // namespace DlssNr::AmdBridge
