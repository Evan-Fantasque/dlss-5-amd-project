#pragma once
#include <cstdint>

namespace AmdPreSr
{
// CPU-only admission policy, serialized by AmdBridge::frameMutex. This is not
// menu detection: it deliberately also excludes native-size temporal AA.
class UpscaleGate
{
  public:
    struct Decision { bool allowed; bool changed; const char* reason; };
    Decision Observe(bool required, unsigned width, unsigned height, unsigned outputWidth, unsigned outputHeight)
    {
        const bool valid = width && height && outputWidth && outputHeight;
        const bool enlarged = valid && width <= outputWidth && height <= outputHeight &&
                              (width < outputWidth || height < outputHeight);
        const bool allowed = !required || enlarged;
        const unsigned state = !required ? 0 : !valid ? 1 : enlarged ? 2 : 3;
        const bool changed = !seen || state != previousState || width != previousWidth || height != previousHeight ||
                             outputWidth != previousOutputWidth || outputHeight != previousOutputHeight;
        seen = true;
        previousState = state;
        previousWidth = width;
        previousHeight = height;
        previousOutputWidth = outputWidth;
        previousOutputHeight = outputHeight;
        if (!allowed) ++blockedCalls;
        return { allowed, changed, state == 0 ? "sr_gate_disabled" : state == 1 ? "sr_extent_unknown" :
                                  state == 2 ? "sr_upscaling" : "sr_not_upscaling" };
    }
    std::uint64_t BlockedCalls() const { return blockedCalls; }
  private:
    bool seen = false;
    unsigned previousState = 0, previousWidth = 0, previousHeight = 0;
    unsigned previousOutputWidth = 0, previousOutputHeight = 0;
    std::uint64_t blockedCalls = 0;
};
}
