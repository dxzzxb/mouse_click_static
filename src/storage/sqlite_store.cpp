#include "storage/sqlite_store.h"
#include "platform/windows_helpers.h"
#include <fstream>
#include <limits>
#include <stdexcept>
#include <cstring>

namespace mouse {
namespace {
struct sqlite3;
struct sqlite3_stmt;
constexpr int kOk = 0, kRow = 100, kDone = 101;
struct Api {
    HMODULE module = nullptr;
    int (__cdecl* open_v2)(const char*, sqlite3**, int, const char*) = nullptr;
    int (__cdecl* close)(sqlite3*) = nullptr;
    const char* (__cdecl* errmsg)(sqlite3*) = nullptr;
    int (__cdecl* exec)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**) = nullptr;
    int (__cdecl* busy_timeout)(sqlite3*, int) = nullptr;
    int (__cdecl* prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = nullptr;
    int (__cdecl* finalize)(sqlite3_stmt*) = nullptr;
    int (__cdecl* step)(sqlite3_stmt*) = nullptr;
    int (__cdecl* reset)(sqlite3_stmt*) = nullptr;
    int (__cdecl* clear_bindings)(sqlite3_stmt*) = nullptr;
    int (__cdecl* bind_text)(sqlite3_stmt*, int, const char*, int, void (*)(void*)) = nullptr;
    int (__cdecl* bind_int64)(sqlite3_stmt*, int, int64_t) = nullptr;
    int64_t (__cdecl* column_int64)(sqlite3_stmt*, int) = nullptr;
    const unsigned char* (__cdecl* column_text)(sqlite3_stmt*, int) = nullptr;
    template<class T> void Load(T& fn, const char* name) {
        auto address = GetProcAddress(module, name);
        if (!address) throw std::runtime_error(std::string("SQLite export missing: ") + name);
        static_assert(sizeof(fn) == sizeof(address));
        std::memcpy(&fn, &address, sizeof(fn));
    }
    Api() {
        module = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw std::runtime_error("Windows SQLite is unavailable (Windows 10+ required)");
        try {
#define LOAD(name) Load(name, "sqlite3_" #name)
            LOAD(open_v2); LOAD(close); LOAD(errmsg); LOAD(exec); LOAD(busy_timeout);
            LOAD(prepare_v2); LOAD(finalize); LOAD(step); LOAD(reset); LOAD(clear_bindings);
            LOAD(bind_text); LOAD(bind_int64); LOAD(column_int64); LOAD(column_text);
#undef LOAD
        } catch (...) { FreeLibrary(module); throw; }
    }
    ~Api() { FreeLibrary(module); }
};
Api& SQLite() { static Api api; return api; }
struct Statement {
    sqlite3_stmt* value = nullptr;
    explicit Statement(sqlite3* db, const char* sql) {
        if (SQLite().prepare_v2(db, sql, -1, &value, nullptr) != kOk) throw std::runtime_error(SQLite().errmsg(db));
    }
    ~Statement() { SQLite().finalize(value); }
    void Text(int index, const std::string& text) {
        if (SQLite().bind_text(value, index, text.c_str(), -1, reinterpret_cast<void (*)(void*)>(static_cast<intptr_t>(-1))) != kOk)
            throw std::runtime_error("SQLite text binding failed");
    }
    void Number(int index, int64_t number) {
        if (SQLite().bind_int64(value, index, number) != kOk) throw std::runtime_error("SQLite number binding failed");
    }
    void Reset() { SQLite().reset(value); SQLite().clear_bindings(value); }
};
std::string TextColumn(sqlite3_stmt* stmt, int column) {
    const auto* text = SQLite().column_text(stmt, column);
    return text ? reinterpret_cast<const char*>(text) : "";
}
} // namespace

struct SqliteStore::Impl {
    sqlite3* db = nullptr;
    bool readOnly = false;
    ~Impl() { if (db) SQLite().close(db); }
    void Exec(const char* sql) {
        if (SQLite().exec(db, sql, nullptr, nullptr, nullptr) != kOk) throw std::runtime_error(SQLite().errmsg(db));
    }
};
SqliteStore::SqliteStore(const std::filesystem::path& file, bool readOnly) : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_;
    p.readOnly = readOnly;
    const auto path = Utf8(file.wstring());
    // One connection per owning thread, full-mutex as an additional safeguard.
    if (SQLite().open_v2(path.c_str(), &p.db, (readOnly ? 0x1 : 0x2 | 0x4) | 0x10000, nullptr) != kOk)
        throw std::runtime_error(p.db ? SQLite().errmsg(p.db) : "Cannot open statistics database");
    SQLite().busy_timeout(p.db, readOnly ? 250 : 1500);
    Statement version(p.db, "PRAGMA user_version");
    if (SQLite().step(version.value) != kRow) throw std::runtime_error("Cannot read database version");
    auto schema = SQLite().column_int64(version.value, 0);
    if (schema > 1) throw std::runtime_error("Database was created by a newer version; upgrade the application");
    if (readOnly) {
        if (schema != 1) throw std::runtime_error("Statistics database is not initialized yet");
        return;
    }
    // Finish the schema read before switching journal modes.
    SQLite().reset(version.value);
    p.Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
    p.Exec("BEGIN IMMEDIATE;"
           "CREATE TABLE IF NOT EXISTS applications(app_key TEXT PRIMARY KEY, name TEXT NOT NULL);"
           "CREATE TABLE IF NOT EXISTS daily_stats("
           "date TEXT NOT NULL, app_key TEXT NOT NULL REFERENCES applications(app_key),"
           "left_clicks INTEGER NOT NULL DEFAULT 0, right_clicks INTEGER NOT NULL DEFAULT 0,"
           "middle_clicks INTEGER NOT NULL DEFAULT 0, x1_clicks INTEGER NOT NULL DEFAULT 0, x2_clicks INTEGER NOT NULL DEFAULT 0,"
           "wheel_up INTEGER NOT NULL DEFAULT 0, wheel_down INTEGER NOT NULL DEFAULT 0,"
           "wheel_left INTEGER NOT NULL DEFAULT 0, wheel_right INTEGER NOT NULL DEFAULT 0,"
           "PRIMARY KEY(date,app_key)) WITHOUT ROWID;"
           "PRAGMA user_version=1; COMMIT;");
}
SqliteStore::~SqliteStore() = default;
void SqliteStore::ClearAll() {
    auto& p = *impl_;
    if (p.readOnly) throw std::runtime_error("Read-only database");
    p.Exec("BEGIN IMMEDIATE");
    try {
        p.Exec("DELETE FROM daily_stats; DELETE FROM applications; COMMIT;");
    } catch (...) {
        SQLite().exec(p.db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
void SqliteStore::Save(const Batch& batch) {
    if (batch.empty()) return;
    auto& p = *impl_;
    if (p.readOnly) throw std::runtime_error("Read-only database");
    p.Exec("BEGIN IMMEDIATE");
    try {
        Statement app(p.db, "INSERT INTO applications(app_key,name) VALUES(?,?) ON CONFLICT(app_key) DO UPDATE SET name=excluded.name");
        // Saturating integer arithmetic avoids SQLite converting large sums to floating point.
        Statement stats(p.db,
            "INSERT INTO daily_stats VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(date,app_key) DO UPDATE SET "
            "left_clicks=left_clicks+min(excluded.left_clicks,9223372036854775807-left_clicks),"
            "right_clicks=right_clicks+min(excluded.right_clicks,9223372036854775807-right_clicks),"
            "middle_clicks=middle_clicks+min(excluded.middle_clicks,9223372036854775807-middle_clicks),"
            "x1_clicks=x1_clicks+min(excluded.x1_clicks,9223372036854775807-x1_clicks),"
            "x2_clicks=x2_clicks+min(excluded.x2_clicks,9223372036854775807-x2_clicks),"
            "wheel_up=wheel_up+min(excluded.wheel_up,9223372036854775807-wheel_up),"
            "wheel_down=wheel_down+min(excluded.wheel_down,9223372036854775807-wheel_down),"
            "wheel_left=wheel_left+min(excluded.wheel_left,9223372036854775807-wheel_left),"
            "wheel_right=wheel_right+min(excluded.wheel_right,9223372036854775807-wheel_right)");
        for (const auto& [key, row] : batch) {
            app.Text(1, key.app); app.Text(2, row.name);
            if (SQLite().step(app.value) != kDone) throw std::runtime_error(SQLite().errmsg(p.db));
            app.Reset();
            stats.Text(1, key.date); stats.Text(2, key.app);
            for (int i = 0; i < 5; ++i) stats.Number(i + 3, row.counts.clicks[i]);
            stats.Number(8, row.counts.wheelUp); stats.Number(9, row.counts.wheelDown);
            stats.Number(10, row.counts.wheelLeft); stats.Number(11, row.counts.wheelRight);
            if (SQLite().step(stats.value) != kDone) throw std::runtime_error(SQLite().errmsg(p.db));
            stats.Reset();
        }
        p.Exec("COMMIT");
    } catch (...) {
        SQLite().exec(p.db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
std::vector<StoredRow> SqliteStore::Read(const std::string& from, const std::string& to) {
    Statement stmt(impl_->db,
        "SELECT s.date,s.app_key,a.name,s.left_clicks,s.right_clicks,s.middle_clicks,s.x1_clicks,s.x2_clicks,"
        "s.wheel_up,s.wheel_down,s.wheel_left,s.wheel_right FROM daily_stats s JOIN applications a USING(app_key) "
        "WHERE s.date>=? AND s.date<=? ORDER BY s.date,s.app_key");
    stmt.Text(1, from); stmt.Text(2, to);
    std::vector<StoredRow> rows;
    int status;
    while ((status = SQLite().step(stmt.value)) == kRow) {
        StoredRow row;
        row.key = {TextColumn(stmt.value, 0), TextColumn(stmt.value, 1)};
        row.name = TextColumn(stmt.value, 2);
        for (int i = 0; i < 5; ++i) row.counts.clicks[i] = SQLite().column_int64(stmt.value, i + 3);
        row.counts.wheelUp = SQLite().column_int64(stmt.value, 8);
        row.counts.wheelDown = SQLite().column_int64(stmt.value, 9);
        row.counts.wheelLeft = SQLite().column_int64(stmt.value, 10);
        row.counts.wheelRight = SQLite().column_int64(stmt.value, 11);
        rows.push_back(std::move(row));
    }
    if (status != kDone) throw std::runtime_error(SQLite().errmsg(impl_->db));
    return rows;
}
LifetimeStats SqliteStore::ReadLifetime() {
    Statement stmt(impl_->db, "SELECT date,left_clicks,right_clicks,middle_clicks,x1_clicks,x2_clicks FROM daily_stats ORDER BY date");
    LifetimeStats result;
    int status;
    // Fold without SQL SUM: Counts::Add preserves exact integers and saturates on overflow.
    while ((status = SQLite().step(stmt.value)) == kRow) {
        if (result.firstDate.empty()) result.firstDate = TextColumn(stmt.value, 0);
        Counts row;
        for (int i = 0; i < 5; ++i) row.clicks[i] = SQLite().column_int64(stmt.value, i + 1);
        result.counts.Add(row);
    }
    if (status != kDone) throw std::runtime_error(SQLite().errmsg(impl_->db));
    return result;
}
void SqliteStore::ExportCsv(const std::filesystem::path& file, const std::string& from, const std::string& to) {
    auto rows = Read(from, to);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Cannot create export file");
    stream << "\xEF\xBB\xBF" << "date,application,application_path,left,right,middle,side1,side2,wheel_up_delta,wheel_down_delta,wheel_left_delta,wheel_right_delta\r\n";
    for (const auto& row : rows) {
        stream << CsvCell(row.key.date) << ',' << CsvCell(row.name) << ',' << CsvCell(row.key.app);
        for (auto value : row.counts.clicks) stream << ',' << value;
        stream << ',' << row.counts.wheelUp << ',' << row.counts.wheelDown << ',' << row.counts.wheelLeft << ',' << row.counts.wheelRight << "\r\n";
    }
    stream.flush();
    if (!stream) throw std::runtime_error("Export write failed");
}
void SqliteStore::ExecuteForTesting(const char* sql) { impl_->Exec(sql); }
} // namespace mouse

