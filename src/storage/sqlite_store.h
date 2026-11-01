#pragma once
#include "core/counters.h"
#include <filesystem>
#include <memory>
#include <vector>

namespace mouse {
struct StoredRow {
    Key key;
    std::string name;
    Counts counts;
};
struct LifetimeStats {
    Counts counts;
    std::string firstDate;
};
// Uses the Windows-shipped SQLite ABI. No bundled SQL engine or third-party runtime.
class SqliteStore {
public:
    explicit SqliteStore(const std::filesystem::path& file, bool readOnly = false);
    ~SqliteStore();
    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;
    void Save(const Batch& batch);
    void ClearAll();
    std::vector<StoredRow> Read(const std::string& from, const std::string& to);
    LifetimeStats ReadLifetime();
    void ExportCsv(const std::filesystem::path& file, const std::string& from, const std::string& to);
    void ExecuteForTesting(const char* sql);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mouse

