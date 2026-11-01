#pragma once
#include "storage/sqlite_store.h"
#include <unordered_map>

namespace mouse {
struct AppMetrics {
    std::string key, name;
    Counts selected, previous, period;
    std::vector<Counts> days;
};
struct DashboardModel {
    std::string date, from;
    int range = 7;
    Counts selected, previous, period;
    std::vector<std::string> dates;
    std::vector<Counts> days;
    std::vector<AppMetrics> apps;
    std::unordered_map<std::string, size_t> appIndex;
    size_t signature = 0;
    size_t peakDay = 0;
    double DailyAverage() const { return static_cast<double>(period.TotalClicks()) / range; }
    const AppMetrics* FindApp(const std::string& key) const {
        auto it = appIndex.find(key);
        return it == appIndex.end() ? nullptr : &apps[it->second];
    }
};
DashboardModel BuildDashboardModel(const std::vector<StoredRow>& rows, const std::string& date, int range);
bool SameCounts(const Counts& a, const Counts& b);
}
