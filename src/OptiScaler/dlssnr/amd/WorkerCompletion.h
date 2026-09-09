#pragma once
#include <Windows.h>
#include <cstdint>

namespace AmdPreSr
{
// ABI of the hash-checked v0.2.14 runtime only. The early inline marker
// precedes hipMemcpy of the flag buffer and watchdog/statistics updates.
// RVA 0xF28C exchanges the final marker after that bookkeeping. Staging
// recreation resets it to -1, which must never compare as UINT_MAX >= job.
struct WorkerCompletion
{
    UINT inference = 0;
    UINT retired = UINT_MAX;
    bool Complete(UINT job) const { return inference >= job && retired == job; }
    static WorkerCompletion Read(HMODULE module)
    {
        auto read = [module](size_t rva) {
            return static_cast<UINT>(InterlockedCompareExchange(
                reinterpret_cast<volatile LONG*>(reinterpret_cast<uintptr_t>(module) + rva), 0, 0));
        };
        const auto retired = read(0x76d60);
        return { read(0x76c14), retired };
    }
};
}
