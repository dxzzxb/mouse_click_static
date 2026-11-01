#include "core/input_merger.h"
#include <algorithm>
#include <limits>

namespace mouse {
std::vector<InputEvent> InputMerger::Drain(uint32_t now, bool all) {
    std::vector<InputEvent> out;
    for (auto& entry : entries_) if (!entry.committed && (all || now - entry.arrived >= 60)) {
        out.push_back(entry.event); entry.committed = true;
    }
    while (!entries_.empty() && (all || now - entries_.front().arrived >= 2000)) entries_.pop_front();
    return out;
}
std::vector<InputEvent> InputMerger::Push(InputEvent event, uint32_t now) {
    auto out = Drain(now);
    constexpr uint16_t masks[]{kLeftDown, kRightDown, kMiddleDown, kX1Down, kX2Down, kWheel, kHWheel};
    for (auto mask : masks) if (event.flags & mask) {
        auto single = event; single.flags = mask;
        if (mask != kWheel && mask != kHWheel) single.wheel = 0;
        auto nearest = entries_.end(); uint32_t distance = 33;
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->paired || it->event.raw == event.raw || it->event.flags != mask || it->event.wheel != single.wheel) continue;
            const auto forward = single.timestamp - it->event.timestamp;
            const auto backward = it->event.timestamp - single.timestamp;
            const auto delta = (std::min)(forward, backward); // DWORD wrap-safe.
            if (delta < distance) { nearest = it; distance = delta; }
        }
        if (nearest != entries_.end()) {
            nearest->paired = true;
            if (!nearest->committed) { out.push_back(single); nearest->committed = true; }
        } else {
            if (single.raw) out.push_back(single);
            entries_.push_back({std::move(single), now, false, event.raw});
        }
    }
    return out;
}
} // namespace mouse
