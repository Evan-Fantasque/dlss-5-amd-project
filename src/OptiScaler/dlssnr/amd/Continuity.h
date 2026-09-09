#pragma once
#include <cstdint>

namespace AmdPreSr
{
// Owned by Backend's mutex. Input activity is independent of when a completed
// GPU job is noticed. Pending reasons survive all early returns and failures.
class Continuity
{
  public:
    enum Reason : unsigned { Game = 1, Gap = 2, Bypass = 4 };
    std::uint64_t Observe(std::uint64_t now, bool reset)
    {
        if (calls && now - lastInput > 250)
            pending |= Gap;
        if (reset)
            pending |= Game;
        lastInput = now;
        return ++calls;
    }
    void Skipped() { pending |= Bypass; }
    unsigned Pending() const { return pending; }
    // Only after a native job was accepted. No GPU work is changed here.
    void Accepted(unsigned reasons) { pending &= ~reasons; }
    std::uint64_t Calls() const { return calls; }
  private:
    std::uint64_t calls = 0, lastInput = 0;
    unsigned pending = 0;
};
}
