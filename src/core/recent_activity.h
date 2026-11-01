#pragma once
#include "core/counters.h"
#include <algorithm>
#include <limits>

namespace mouse {
struct RecentSummary {
    int64_t halfHour = 0, hour = 0;
    uint64_t observedMs = 0;
};

// At most 60 minute aggregates in RAM. No individual events, titles or app paths.
// The collector owns this clock; reopening the dashboard does not reset the hour.
class RecentActivity {
public:
    explicit RecentActivity(uint64_t startedAt = 0) : startedAt_(startedAt) {}
    void Record(uint64_t now, uint16_t flags) {
        int64_t clicks = 0;
        for (auto mask : {kLeftDown, kRightDown, kMiddleDown, kX1Down, kX2Down}) if (flags & mask) ++clicks;
        if (!clicks) return;
        Prune(now / 60000);
        auto& bucket = minutes_[now / 60000]; bucket = Add(bucket, clicks);
    }
    RecentSummary Snapshot(uint64_t now) {
        const auto minute = now / 60000;
        Prune(minute);
        RecentSummary out;
        out.observedMs = now >= startedAt_ ? (std::min)(now - startedAt_, uint64_t{3600000}) : 0;
        for (const auto& [key, clicks] : minutes_) if (key <= minute) {
            out.hour = Add(out.hour, clicks);
            if (minute - key < 30) out.halfHour = Add(out.halfHour, clicks);
        }
        return out;
    }
    // Only discard the pre-clear checkpoint after the storage transaction succeeds.
    // Input arriving while deletion is pending remains in the new statistics.
    void Discard(const RecentActivity& checkpoint, uint64_t clearedAt) {
        for (const auto& [minute, clicks] : checkpoint.minutes_) if (auto found = minutes_.find(minute); found != minutes_.end()) {
            found->second = (std::max)(int64_t{0}, found->second - clicks);
            if (!found->second) minutes_.erase(found);
        }
        startedAt_ = clearedAt;
    }
private:
    static int64_t Add(int64_t a, int64_t b) {
        const auto max = (std::numeric_limits<int64_t>::max)();
        return b > max - a ? max : a + b;
    }
    void Prune(uint64_t minute) {
        while (!minutes_.empty() && minutes_.begin()->first <= minute && minute - minutes_.begin()->first >= 60)
            minutes_.erase(minutes_.begin());
    }
    uint64_t startedAt_;
    std::map<uint64_t, int64_t> minutes_;
};

enum class PulseMood { Warming, Quiet, Casual, Busy, Turbo, Starcraft, Marathon };
inline PulseMood ActivityMood(const RecentSummary& recent) {
    if (recent.observedMs < 300000) return PulseMood::Warming;
    if (recent.hour >= 10000) return PulseMood::Marathon;
    if (recent.halfHour >= 6000) return PulseMood::Starcraft;
    if (recent.halfHour >= 3600) return PulseMood::Turbo;
    if (recent.halfHour >= 1800) return PulseMood::Busy;
    return recent.halfHour ? PulseMood::Casual : PulseMood::Quiet;
}
inline const wchar_t* ActivityHeadline(PulseMood mood) {
    switch (mood) {
    case PulseMood::Warming: return L"先别急，鼠鼠还在热身。";
    case PulseMood::Quiet: return L"鼠标已躺平，精神状态遥遥领先。";
    case PulseMood::Casual: return L"点到为止，主打一个松弛感。";
    case PulseMood::Busy: return L"鼠标：这班非我上不可吗？";
    case PulseMood::Turbo: return L"手指在输出，鼠标在渡劫。";
    case PulseMood::Starcraft: return L"你的 APM 牛炸了，建议去打星际。";
    case PulseMood::Marathon: return L"一小时破万，鼠标准备单开族谱。";
    }
    return L"鼠鼠电台，持续营业。";
}
} // namespace mouse
