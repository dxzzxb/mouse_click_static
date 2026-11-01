#include "core/counters.h"
#include <limits>

namespace mouse {
static int64_t AddSafe(int64_t a, int64_t b) {
    const auto limit = (std::numeric_limits<int64_t>::max)();
    return b > limit - a ? limit : a + b;
}
int64_t Counts::TotalClicks() const {
    int64_t total = 0;
    for (auto value : clicks) total = AddSafe(total, value);
    return total;
}
bool Counts::Empty() const {
    return TotalClicks() == 0 && !wheelUp && !wheelDown && !wheelLeft && !wheelRight;
}
void Counts::Add(const Counts& other) {
    for (size_t i = 0; i < clicks.size(); ++i) clicks[i] = AddSafe(clicks[i], other.clicks[i]);
    wheelUp = AddSafe(wheelUp, other.wheelUp);
    wheelDown = AddSafe(wheelDown, other.wheelDown);
    wheelLeft = AddSafe(wheelLeft, other.wheelLeft);
    wheelRight = AddSafe(wheelRight, other.wheelRight);
}
void Merge(Batch& target, const Batch& source) {
    for (const auto& [key, row] : source) {
        auto& out = target[key];
        out.name = row.name;
        out.counts.Add(row.counts);
    }
}
bool Counter::Record(const std::string& date, const std::string& app, const std::string& name,
                     uint16_t flags, int16_t wheelDelta) {
    if (paused_ || !(flags & kRelevantFlags)) return false;
    Counts add;
    constexpr uint16_t masks[]{kLeftDown, kRightDown, kMiddleDown, kX1Down, kX2Down};
    for (size_t i = 0; i < add.clicks.size(); ++i) if (flags & masks[i]) add.clicks[i] = 1;
    const auto delta = static_cast<int>(wheelDelta); // -32768 must be negated AFTER promotion.
    if (flags & kWheel) {
        if (delta > 0) add.wheelUp = delta;
        else add.wheelDown = -delta;
    }
    if (flags & kHWheel) {
        if (delta > 0) add.wheelRight = delta;
        else add.wheelLeft = -delta;
    }
    if (add.Empty()) return false;
    auto& row = pending_[Key{date, app}];
    row.name = name;
    row.counts.Add(add);
    return true;
}
Batch Counter::TakePending() {
    Batch result;
    result.swap(pending_);
    return result;
}
} // namespace mouse

