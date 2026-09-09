#include "Continuity.h"
#include "Performance.h"
#include <iostream>
#include <stdexcept>

void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
int main()
{
    AmdPreSr::Continuity h;
    // A job finishes while the consumer is paused. There is deliberately no
    // completion callback capable of changing the input-activity clock.
    h.Observe(1000, false);
    h.Accepted(h.Pending());
    h.Observe(5500, false);
    require(h.Pending() & AmdPreSr::Continuity::Gap, "load pause was hidden");
    h.Accepted(h.Pending());
    h.Observe(5516, false);
    require(!h.Pending(), "ordinary next frame reset history");
    // Only the busy/invalid frame contains Reset; later frames don't repeat it.
    h.Observe(5532, true);
    h.Skipped();
    h.Observe(5548, false);
    h.Skipped();
    h.Observe(5564, false);
    require((h.Pending() & (AmdPreSr::Continuity::Game | AmdPreSr::Continuity::Bypass)) ==
            (AmdPreSr::Continuity::Game | AmdPreSr::Continuity::Bypass), "one-frame reset lost on bypass");
    h.Accepted(h.Pending());
    h.Observe(5580, false);
    require(!h.Pending(), "accepted reset consumed more than once");
    h.Observe(5830, false);
    require(!h.Pending(), "250ms boundary changed");
    h.Observe(6081, false);
    require(h.Pending() & AmdPreSr::Continuity::Gap, "251ms gap not detected");
    const auto missing = std::filesystem::temp_directory_path() /
                         (L"absent-test4-config-" + std::to_wstring(GetCurrentProcessId()));
    require(!AmdPreSr::Perf::Options::Read(missing).asyncSingle, "missing INI must default to sync");
    std::cout << "PASS: delayed observation, busy reset, bypass, exactly-once acceptance, gap boundary, sync default\n";
}
