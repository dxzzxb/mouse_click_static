#include "core/dashboard_model.h"
#include "platform/windows_helpers.h"
#include <algorithm>
#include <stdexcept>

namespace mouse {
bool SameCounts(const Counts& a, const Counts& b) {
    return a.clicks == b.clicks && a.wheelUp == b.wheelUp && a.wheelDown == b.wheelDown && a.wheelLeft == b.wheelLeft && a.wheelRight == b.wheelRight;
}
DashboardModel BuildDashboardModel(const std::vector<StoredRow>& rows, const std::string& date, int range) {
    if (range < 1 || range > 366) throw std::invalid_argument("Invalid dashboard range");
    DashboardModel model;
    model.date = date; model.range = range; model.from = ShiftDate(date, 1 - range);
    model.signature = std::hash<std::string>{}(date + std::to_string(range));
    std::unordered_map<std::string, size_t> dayIndex;
    const auto yesterday = ShiftDate(date, -1);
    for (int i = 0; i < range; ++i) {
        auto day = ShiftDate(model.from, i);
        dayIndex.emplace(day, model.dates.size()); model.dates.push_back(std::move(day));
    }
    model.days.resize(range);
    // Only applications active on the selected date need a row and an inspector series.
    for (const auto& row : rows) if (row.key.date == date) {
        auto [it, inserted] = model.appIndex.emplace(row.key.app, model.apps.size());
        if (inserted) model.apps.push_back({row.key.app, row.name, {}, {}, {}, std::vector<Counts>(range)});
        model.apps[it->second].selected.Add(row.counts);
        model.selected.Add(row.counts);
    }
    const auto mix = [&model](size_t value) { model.signature ^= value + 0x9e3779b9 + (model.signature << 6) + (model.signature >> 2); };
    for (const auto& row : rows) {
        if (row.key.date == yesterday) model.previous.Add(row.counts);
        auto app = model.appIndex.find(row.key.app);
        if (app != model.appIndex.end() && row.key.date == yesterday) model.apps[app->second].previous.Add(row.counts);
        auto day = dayIndex.find(row.key.date);
        if (day == dayIndex.end()) continue;
        model.period.Add(row.counts); model.days[day->second].Add(row.counts);
        if (app != model.appIndex.end()) {
            auto& metrics = model.apps[app->second];
            metrics.period.Add(row.counts); metrics.days[day->second].Add(row.counts);
        }
        mix(std::hash<std::string>{}(row.key.date + row.key.app + row.name));
        for (auto value : row.counts.clicks) mix(static_cast<size_t>(value));
        mix(static_cast<size_t>(row.counts.wheelUp)); mix(static_cast<size_t>(row.counts.wheelDown));
        mix(static_cast<size_t>(row.counts.wheelLeft)); mix(static_cast<size_t>(row.counts.wheelRight));
    }
    for (size_t i = 1; i < model.days.size(); ++i)
        if (model.days[i].TotalClicks() >= model.days[model.peakDay].TotalClicks()) model.peakDay = i;
    std::sort(model.apps.begin(), model.apps.end(), [](const auto& a, const auto& b) {
        const auto left = a.selected.TotalClicks(), right = b.selected.TotalClicks();
        return left == right ? a.key < b.key : left > right;
    });
    model.appIndex.clear();
    for (size_t i = 0; i < model.apps.size(); ++i) model.appIndex.emplace(model.apps[i].key, i);
    return model;
}
}
