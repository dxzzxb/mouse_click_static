#pragma once
#include "core/counters.h"
#include <deque>
#include <vector>

namespace mouse {
struct InputEvent {
    uint32_t timestamp = 0;
    uint16_t flags = 0;
    int16_t wheel = 0;
    std::string date, app, name;
    bool raw = true;
};

// Raw Input is immediate. The compatibility channel waits briefly for its raw twin.
// Pair individual button events one-to-one; do not debounce rapid clicks from one source.
class InputMerger {
public:
    std::vector<InputEvent> Push(InputEvent event, uint32_t now);
    std::vector<InputEvent> Drain(uint32_t now, bool all = false);
    bool Pending() const { for (const auto& entry : entries_) if (!entry.committed) return true; return false; }
private:
    struct Entry { InputEvent event; uint32_t arrived; bool paired = false, committed = false; };
    std::deque<Entry> entries_;
};
} // namespace mouse
