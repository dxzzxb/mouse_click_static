#pragma once
#include "core/counters.h"
#include "core/clear_state.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace mouse {
class Persistence {
public:
    explicit Persistence(std::filesystem::path path);
    ~Persistence();
    void Submit(Batch batch);
    uint64_t RequestClear();
    ClearState Clearing(uint64_t ticket) const;
    bool Stop();
    bool Failed() const { return failed_.load(); }
    bool Ready() const { return ready_.load(); }
    std::string Error() const;
private:
    void Run();
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Batch pending_, afterClear_;
    uint64_t clearTicket_ = 0;
    ClearState clearState_ = ClearState::Unknown;
    bool clearRequested_ = false;
    bool stopping_ = false;
    std::string error_;
    std::atomic<bool> failed_{false}, ready_{false};
    std::thread thread_;
};
} // namespace mouse

