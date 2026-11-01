#include "collector/persistence.h"
#include "storage/sqlite_store.h"
#include <chrono>
#include <memory>

namespace mouse {
Persistence::Persistence(std::filesystem::path path) : path_(std::move(path)), thread_([this] { Run(); }) {}
Persistence::~Persistence() { Stop(); }
void Persistence::Submit(Batch batch) {
    if (batch.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Merge(clearState_ == ClearState::Pending ? afterClear_ : pending_, batch);
    }
    cv_.notify_one();
}
uint64_t Persistence::RequestClear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return 0;
    if (clearState_ == ClearState::Pending) return clearTicket_;
    clearState_ = ClearState::Pending; clearRequested_ = true; ++clearTicket_;
    cv_.notify_one();
    return clearTicket_;
}
ClearState Persistence::Clearing(uint64_t ticket) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ticket == clearTicket_ && ticket ? clearState_ : ClearState::Unknown;
}
std::string Persistence::Error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}
bool Persistence::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    return !failed_;
}
void Persistence::Run() {
    std::unique_ptr<SqliteStore> database;
    bool retry = false;
    for (;;) {
        Batch batch;
        bool stop = false, clear = false, success = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (database && !retry) cv_.wait(lock, [this] { return stopping_ || clearRequested_ || !pending_.empty(); });
            if (retry) cv_.wait_for(lock, std::chrono::seconds(30), [this] { return stopping_ || clearRequested_; });
            stop = stopping_;
            clear = clearRequested_; clearRequested_ = false;
            if (!clear) batch.swap(pending_);
        }
        try {
            if (!database) database = std::make_unique<SqliteStore>(path_);
            ready_ = true;
            if (clear) database->ClearAll(); else database->Save(batch);
            success = true;
            failed_ = false;
            retry = false;
            { std::lock_guard<std::mutex> lock(mutex_); error_.clear(); }
        } catch (const std::exception& e) {
            failed_ = true;
            retry = true;
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = e.what();
            // An unsuccessful transaction is rolled back; requeue deltas exactly once.
            if (!clear) Merge(pending_, batch);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (clear) {
                // The worker has finished any earlier save. Old pending deltas are discarded
                // only after a successful deletion; later deltas belong to the new statistics.
                if (success) pending_.clear();
                Merge(pending_, afterClear_); afterClear_.clear();
                clearState_ = success ? ClearState::Succeeded : ClearState::Failed;
            }
            if (stop && ((!success && !clear && !clearRequested_) || (pending_.empty() && !clearRequested_))) return;
        }
    }
}
} // namespace mouse
