#pragma once
#include <cstdint>

namespace mouse {
// A short disconnect can be an elevation handoff. A known collector that stays
// absent should close its dashboard, including when the dashboard is minimized.
class CollectorWatch {
public:
    bool ShouldClose(bool connected, bool handingOff, uint64_t now) {
        if (connected) { seen_ = true; missing_ = false; return false; }
        if (!seen_ || handingOff) { missing_ = false; return false; }
        if (!missing_) { missing_ = true; missingAt_ = now; return false; }
        return now >= missingAt_ && now - missingAt_ >= 8000;
    }
private:
    bool seen_ = false, missing_ = false;
    uint64_t missingAt_ = 0;
};
} // namespace mouse
