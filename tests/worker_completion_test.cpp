#include "WorkerCompletion.h"
#include <array>
#include <iostream>
#include <stdexcept>
using AmdPreSr::WorkerCompletion;
void require(bool b) { if (!b) throw std::runtime_error("Worker completion invariant failed"); }
int main()
{
    // A resized runtime reuses job 1. The -1 marker is not a completed job,
    // and a larger marker from the previous extent cannot retire the new job.
    require(!WorkerCompletion{1, UINT_MAX}.Complete(1));
    require(!WorkerCompletion{1, 300}.Complete(1));
    require(!WorkerCompletion{2, 1}.Complete(2));
    require(!WorkerCompletion{1, 2}.Complete(2));
    require(WorkerCompletion{2, 2}.Complete(2));
    alignas(16) static std::array<unsigned char, 0x77000> image{};
    *reinterpret_cast<UINT*>(image.data()+0x76c14)=2;
    *reinterpret_cast<UINT*>(image.data()+0x76d60)=1;
    require(!WorkerCompletion::Read(reinterpret_cast<HMODULE>(image.data())).Complete(2));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(image.data()+0x76d60),2);
    require(WorkerCompletion::Read(reinterpret_cast<HMODULE>(image.data())).Complete(2));
    std::cout << "PASS: early completion, sentinel, reused job IDs, final publication\n";
}
