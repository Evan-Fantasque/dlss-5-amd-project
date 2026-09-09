#pragma once
#include <Windows.h>
#include <atomic>
#include <iostream>
#include <stdexcept>

// Test-process-only IAT interception. Never included in the product DLL.
// Delay the flags readback after native inference completion, deterministically
// exposing the gap before worker retirement. Runtime bytes on disk are untouched.
namespace TailProbe
{
using Copy = int(*)(void*, const void*, size_t, int);
inline Copy original = nullptr;
inline unsigned char* runtime = nullptr;
inline std::atomic<bool> entered {false}, exited {false};
inline void** slot = nullptr;
inline int Hook(void* dst, const void* src, size_t bytes, int kind)
{
    if (bytes == 20 && kind == 2 && src == *reinterpret_cast<void**>(runtime+0x76bf0) &&
        *reinterpret_cast<volatile UINT*>(runtime+0x76c14) == 2 && !entered.exchange(true))
    {
        std::cout << "tail_probe entered: inference=2 retired="
                  << *reinterpret_cast<volatile int*>(runtime+0x76d60) << std::endl;
        Sleep(250);
        const int result = original(dst, src, bytes, kind);
        exited = true;
        return result;
    }
    return original(dst, src, bytes, kind);
}
inline void Install()
{
    runtime = reinterpret_cast<unsigned char*>(GetModuleHandleW(L"dlssnr_amd_pass1.dll"));
    if (!runtime || runtime[0x55a90] != 0xff || runtime[0x55a91] != 0x25)
        throw std::runtime_error("Unexpected native memcpy thunk");
    slot = reinterpret_cast<void**>(runtime+0x55a96+*reinterpret_cast<int*>(runtime+0x55a92));
    if (slot != reinterpret_cast<void**>(runtime+0x6bd18))
        throw std::runtime_error("Unexpected native memcpy IAT");
    original = reinterpret_cast<Copy>(*slot);
    DWORD old;
    if (!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old)) throw std::runtime_error("IAT protection");
    InterlockedExchangePointer(slot,reinterpret_cast<void*>(Hook));
    DWORD unused; VirtualProtect(slot,sizeof(void*),old,&unused);
}
inline void VerifySubmitted(bool asynchronous)
{
    if (asynchronous) return;
    if (!entered || !exited || *reinterpret_cast<volatile int*>(runtime+0x76d60) != 2)
        throw std::runtime_error("REGRESSION: synchronous Submitted returned before worker retirement");
    std::cout << "tail_probe synchronous retirement PASS" << std::endl;
}
template<class Backend> void VerifyAsync(Backend* backend)
{
    const auto deadline=GetTickCount64()+2000;
    while (!entered && GetTickCount64()<deadline) Sleep(1);
    if (!entered || exited) throw std::runtime_error("Tail probe window unavailable");
    if (backend->Ready()) throw std::runtime_error("REGRESSION: Ready released a worker in its bookkeeping tail");
    if (backend->Shutdown()) throw std::runtime_error("REGRESSION: Shutdown released a worker in its bookkeeping tail");
    std::cout << "tail_probe asynchronous retirement and shutdown PASS" << std::endl;
}
}
