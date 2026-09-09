#pragma once
#include "Performance.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstring>

namespace AmdPreSr
{
// Optional observations only. No CPU wait, native wake-up or watchdog mutation.
// One slot is safe because Backend already serializes resource reuse by both
// native completion and its original D3D12 completion fence.
class StageDiagnostics
{
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12QueryHeap> queries;
    Ptr<ID3D12Resource> readback;
    Ptr<ID3D12Fence> probeFence;
    Perf::Csv stages, gpu;
    bool enabled = false, active = false, resolved = false, submitted = false;
    UINT passes = 0, inputW = 0, inputH = 0, modelW = 0, modelH = 0;
    UINT64 frame = 0, call = 0, serial = 0, target = 0, frequency = 0;
    UINT64 calibrationGpu = 0, calibrationCpu = 0;
    bool calibrated = false, flushAfterCollect = false;
    double submitMs = 0, nextSampleMs = 0;
    std::array<UINT, 3> previousDone {}, previousTimeout {}, previousAbort {};
    bool previousGpu = false;
    static double QpcMs(UINT64 value)
    {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        return double(value) * 1000.0 / double(f.QuadPart);
    }
  public:
    bool Init(ID3D12Device* device, const std::filesystem::path& directory, bool requested)
    {
        if (!requested) return false;
        D3D12_QUERY_HEAP_DESC q { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 8, 0 };
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 8 * sizeof(UINT64);
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries))) ||
            FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&readback))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&probeFence))))
            return false;
        const auto pid = std::to_wstring(GetCurrentProcessId());
        stages.Open(directory / (L"amd_presr_stages_test8_" + pid + L".csv"),
            "tick_ms,qpc_ms,frame,source_call,pass,job,phase,elapsed_post_submit_ms,native_done,native_timeouts,abort_word_raw,gpu_list_complete,runtime_field_76c40_unverified");
        gpu.Open(directory / (L"amd_presr_gpu_test8_" + pid + L".csv"),
            "tick_ms,frame,source_call,pass_count,input_w,input_h,model_w,model_h,valid,gpu_total_ms,gpu_prepare_ms,gpu_native1_ms,gpu_native2_ms,gpu_native3_ms,gpu_post_ms,gpu_start_minus_post_submit_ms");
        enabled = true;
        return true;
    }
    bool Enabled() const { return enabled; }
    bool Active() const { return active; }
    void Begin(ID3D12GraphicsCommandList* cmd, UINT64 frameId, UINT64 sourceCall, UINT passCount,
               UINT iw, UINT ih, UINT mw, UINT mh)
    {
        if (!enabled || active) return;
        active = true; resolved = submitted = false; target = 0;
        frame = frameId; call = sourceCall; passes = passCount;
        inputW = iw; inputH = ih; modelW = mw; modelH = mh;
        previousDone = {}; previousAbort = {}; previousGpu = false; flushAfterCollect = false;
        Mark(cmd, 0);
    }
    void Mark(ID3D12GraphicsCommandList* cmd, UINT index)
    {
        if (active && !submitted && index < 8)
            cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    }
    void End(ID3D12GraphicsCommandList* cmd)
    {
        if (!active || submitted) return;
        const UINT count = 2 * passes + 2;
        Mark(cmd, count - 1);
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, count, readback.Get(), 0);
        resolved = true;
    }
    void Submitted(ID3D12CommandQueue* queue)
    {
        if (!active || submitted) return;
        submitted = true; submitMs = Perf::NowMs(); nextSampleMs = submitMs + 50;
        frequency = 0;
        queue->GetTimestampFrequency(&frequency);
        calibrated = SUCCEEDED(queue->GetClockCalibration(&calibrationGpu, &calibrationCpu));
        target = ++serial;
        // Enqueued after Execute, before native Notify. Never wait here: the
        // submitted list itself may need the worker that Notify will awaken.
        if (FAILED(queue->Signal(probeFence.Get(), target))) target = 0;
    }
    bool GpuDone() const
    {
        if (!submitted || !target) return false;
        const auto completed = probeFence->GetCompletedValue();
        return completed != UINT64_MAX && completed >= target;
    }
    void Observe(UINT pass, UINT job, UINT done, UINT timeouts, UINT abortWord, float runtimeValue,
                 const char* phase, bool force = false)
    {
        if (!active || !submitted || pass >= 3) return;
        const auto now = Perf::NowMs();
        const bool complete = GpuDone();
        const bool changed = done != previousDone[pass] || timeouts != previousTimeout[pass] ||
                             abortWord != previousAbort[pass] || complete != previousGpu;
        if (!force && !changed && now < nextSampleMs) return;
        stages.Row(GetTickCount64(), now, frame, call, pass + 1, job, phase, now - submitMs,
                   done, timeouts, abortWord, complete, runtimeValue);
        if (timeouts > previousTimeout[pass]) { flushAfterCollect = true; Flush(); }
        previousDone[pass] = done; previousTimeout[pass] = timeouts; previousAbort[pass] = abortWord;
        previousGpu = complete; nextSampleMs = now + 50;
    }
    // Caller must also have observed native completion, before reusing resources.
    void Collect()
    {
        if (!active || !GpuDone()) return;
        std::array<UINT64, 8> ticks {};
        void* mapped = nullptr;
        D3D12_RANGE range { 0, (2 * passes + 2) * sizeof(UINT64) };
        bool valid = resolved && frequency && SUCCEEDED(readback->Map(0, &range, &mapped));
        if (valid)
        {
            std::memcpy(ticks.data(), mapped, range.End);
            D3D12_RANGE written { 0, 0 }; readback->Unmap(0, &written);
            for (UINT i = 1; i < 2 * passes + 2; ++i) valid &= ticks[i] >= ticks[i - 1];
        }
        auto interval = [&](UINT a, UINT b) { return valid ? double(ticks[b] - ticks[a]) * 1000.0 / frequency : -1.0; };
        const double startRelative = valid && calibrated ?
            QpcMs(calibrationCpu) + (double(ticks[0]) - double(calibrationGpu)) * 1000.0 / frequency - submitMs : -1e9;
        gpu.Row(GetTickCount64(), frame, call, passes, inputW, inputH, modelW, modelH, valid,
                interval(0, 2 * passes + 1), interval(0, 1), interval(1, 2),
                passes >= 2 ? interval(3, 4) : -1.0, passes >= 3 ? interval(5, 6) : -1.0,
                interval(2 * passes, 2 * passes + 1), startRelative);
        if (flushAfterCollect) Flush();
        active = false;
    }
    void Flush() { stages.Flush(); gpu.Flush(); }
};
}
