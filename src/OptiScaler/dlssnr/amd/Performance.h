#pragma once
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace AmdPreSr::Perf
{
inline thread_local double inputReadyWaitMs = -1;
inline thread_local UINT64 bridgeSourceCall = 0;
inline thread_local bool preparedDx11Inputs = false;
struct PreparedDx11Scope
{
    bool previous = preparedDx11Inputs;
    PreparedDx11Scope() { preparedDx11Inputs = true; }
    ~PreparedDx11Scope() { preparedDx11Inputs = previous; }
};
inline double NowMs()
{
    static const double scale = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / f.QuadPart; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * scale;
}
struct Options
{
    // Opt-in because 1:1 temporal AA is also a valid NR use case. Read once; restart after editing INI.
    bool inputReady = false, timerResolution = false, timing = false, asyncSingle = false, requireUpscale = false, diagnosticStages = false, phasedSubmission = false;
    static Options Read(const std::filesystem::path& directory)
    {
        const auto ini = directory / L"amd_presr_perf.ini";
        auto read = [&](const wchar_t* name) { return GetPrivateProfileIntW(L"Performance", name, 0, ini.c_str()) == 1; };
        return { read(L"WaitForDx11Input"), read(L"TimerResolution1ms"), read(L"Timing"),
            GetPrivateProfileIntW(L"Performance", L"AsyncSinglePass", 0, ini.c_str()) != 0, read(L"RequireUpscale"), read(L"DiagnosticStages"), read(L"PhasedSubmission") };
    }
};
// Balanced process request, never a registry or system-wide persistent setting.
// Load the real system DLL explicitly: OptiScaler itself can be named winmm.dll.
class TimerResolution
{
    using Fn = UINT(WINAPI*)(UINT);
    HMODULE module = nullptr;
    Fn end = nullptr;
    bool active = false;
  public:
    bool Start()
    {
        if (active) return true;
        wchar_t directory[MAX_PATH] {};
        if (!GetSystemDirectoryW(directory, MAX_PATH)) return false;
        if (!module)
            module = LoadLibraryExW((std::filesystem::path(directory) / L"winmm.dll").c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return false;
        auto begin = reinterpret_cast<Fn>(GetProcAddress(module, "timeBeginPeriod"));
        end = reinterpret_cast<Fn>(GetProcAddress(module, "timeEndPeriod"));
        active = begin && end && begin(1) == 0;
        return active;
    }
    void Stop() { if (active) { end(1); active = false; } }
    ~TimerResolution() { Stop(); if (module) FreeLibrary(module); }
    TimerResolution() = default;
    TimerResolution(const TimerResolution&) = delete;
    TimerResolution& operator=(const TimerResolution&) = delete;
};
// One reusable event per calling thread. Check the fence, not just a wake-up:
// a registration from an earlier failed wait may still signal this event.
template<class Fence, class Device>
HRESULT WaitForInput(Fence* fence, UINT64 value, Device* device)
{
    struct Event { HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr); ~Event() { if (handle) CloseHandle(handle); } };
    static thread_local Event event;
    if (!event.handle) return HRESULT_FROM_WIN32(GetLastError());
    auto hr = fence->SetEventOnCompletion(value, event.handle);
    if (FAILED(hr)) return hr;
    const double deadline = NowMs() + 5000.0;
    for (;;)
    {
        hr = device->GetDeviceRemovedReason();
        if (FAILED(hr)) return hr;
        const UINT64 completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        if (completed >= value) return S_OK;
        if (NowMs() >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        if (WaitForSingleObject(event.handle, 50) == WAIT_FAILED)
            return HRESULT_FROM_WIN32(GetLastError());
    }
}
// Buffered CSV: no open/close or flush per frame; one row per stage observation.
class Csv
{
    std::ofstream stream;
    UINT count = 0;
  public:
    void Open(const std::filesystem::path& path, const char* header)
    {
        stream.open(path, std::ios::app);
        stream << header << '\n' << std::fixed << std::setprecision(4);
    }
    template<class... T> void Row(T... values)
    {
        if (!stream) return;
        bool first = true;
        auto field = [&](auto value) { if (!first) stream << ','; first = false; stream << value; };
        (field(values), ...);
        stream << '\n';
        if (++count % 120 == 0) stream.flush();
    }
    void Flush() { stream.flush(); }
};
} // namespace AmdPreSr::Perf
