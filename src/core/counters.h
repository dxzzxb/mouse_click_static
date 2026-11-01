#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>

namespace mouse {
// These values deliberately match RAWMOUSE::usButtonFlags, without depending on Windows.h.
constexpr uint16_t kLeftDown = 0x0001, kRightDown = 0x0004, kMiddleDown = 0x0010;
constexpr uint16_t kX1Down = 0x0040, kX2Down = 0x0100;
constexpr uint16_t kWheel = 0x0400, kHWheel = 0x0800;
constexpr uint16_t kRelevantFlags = kLeftDown | kRightDown | kMiddleDown | kX1Down | kX2Down | kWheel | kHWheel;

struct Counts {
    std::array<int64_t, 5> clicks{};
    // Raw deltas preserve partial notches from high-resolution wheels (120 = one notch).
    int64_t wheelUp = 0, wheelDown = 0, wheelLeft = 0, wheelRight = 0;
    int64_t TotalClicks() const;
    bool Empty() const;
    void Add(const Counts& other);
};

struct Key {
    std::string date;
    std::string app;
    bool operator<(const Key& rhs) const { return std::tie(date, app) < std::tie(rhs.date, rhs.app); }
};
struct Row {
    std::string name;
    Counts counts;
};
using Batch = std::map<Key, Row>;
void Merge(Batch& target, const Batch& source);

class Counter {
public:
    bool Record(const std::string& date, const std::string& app, const std::string& name,
                uint16_t flags, int16_t wheelDelta = 0);
    Batch TakePending();
    const Batch& Pending() const { return pending_; }
    void SetPaused(bool paused) { paused_ = paused; }
    bool Paused() const { return paused_; }
private:
    bool paused_ = false;
    Batch pending_;
};
} // namespace mouse

