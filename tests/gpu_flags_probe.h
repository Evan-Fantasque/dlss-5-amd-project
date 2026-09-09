#pragma once
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
// Test-only readback AFTER apply and outer-consumer completion. The native
// worker reads statistics before the phased apply, so its counter can lag one
// job. This probe checks the actual GPU flags, including the last tested job.
inline void CheckGpuFlags(const std::filesystem::path& dir,int frame,UINT passes,bool strict)
{
    std::ofstream csv(dir/L"gpu-flags.csv",std::ios::app);
    if(frame==0)csv<<"frame,pass,job,capture,done,timeouts,last_timeout,spin_iterations\n";
    for(UINT pass=1;pass<=passes;++pass)
    {
        const auto name=L"dlssnr_amd_pass"+std::to_wstring(pass)+L".dll";
        auto base=reinterpret_cast<unsigned char*>(GetModuleHandleW(name.c_str()));
        auto flags=*reinterpret_cast<void**>(base+0x76bf0);
        UINT values[5]{};
        using Copy=int(*)(void*,const void*,size_t,int);
        if(reinterpret_cast<Copy>(base+0x55a90)(values,flags,sizeof(values),2)!=0)
            throw std::runtime_error("Test-only GPU flag readback failed");
        const UINT job=*reinterpret_cast<UINT*>(base+0x76d74);
        csv<<frame<<','<<pass<<','<<job;
        for(auto v:values)csv<<','<<v;
        csv<<'\n';csv.flush();
        if(strict && (values[0]!=job || values[1]!=job || values[2]!=0 || values[3]!=0 || values[4]!=0))
            throw std::runtime_error("Phased GPU marker mismatch, timeout, or unexpected wait-loop spinning");
    }
}
