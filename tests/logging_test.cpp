#include "AmdPreSr.h"
#include "Performance.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void Check(bool value, const char* label) { if (!value) throw std::runtime_error(label); }
int main() {
    try {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "factory");
        Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
        Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "device");
        D3D12_COMMAND_QUEUE_DESC desc {};
        Check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "queue");
        const auto root = std::filesystem::path(".build/logging-test") / std::to_string(GetCurrentProcessId());
        std::string expectedStatus;
        for (int mode : { -1, 0, 1 }) {
            const auto path = std::filesystem::absolute(root / std::to_string(mode));
            Check(!std::filesystem::exists(path), "fresh evidence directory");
            std::filesystem::create_directories(path);
            {
                std::ofstream ini(path / "amd_presr_perf.ini");
                ini << "[Performance]\nTiming=0\nDiagnosticStages=0\nPhasedSubmission=1\n";
                if (mode >= 0) ini << "TextLog=" << mode << '\n';
            }
            const auto options = AmdPreSr::Perf::Options::Read(path);
            Check(options.textLog == (mode != 0), "INI and legacy default");
            Check(options.phasedSubmission && !options.timing, "other settings retained");
            auto backend = new AmdPreSr::Backend(device.Get(), queue.Get(), path);
            Check(!backend->Status().empty(), "status remains available without disk logging");
            if (mode == -1) expectedStatus = backend->Status();
            else Check(backend->Status() == expectedStatus, "same overlay status with logging on/off");
            Check(backend->Shutdown(), "idle shutdown");
            Check(std::filesystem::exists(path / "amd_presr.log") == (mode != 0), "actual log file presence");
            for (const auto& file : std::filesystem::directory_iterator(path))
                Check(file.path().extension() != ".csv", "no performance CSV");
        }
        std::cout << "PASS: TextLog=0 suppresses backend disk logs, preserves status; legacy/default logging and Timing=0 verified\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
