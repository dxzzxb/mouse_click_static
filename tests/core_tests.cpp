#include "core/counters.h"
#include "core/dashboard_model.h"
#include "core/input_merger.h"
#include "core/recent_activity.h"
#include "core/collector_lifecycle.h"
#include "core/click_speed_test.h"
#include "platform/windows_helpers.h"
#include "storage/sqlite_store.h"
#include "collector/persistence.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <chrono>
#include <thread>

namespace {
int checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void TestStartupConfiguration(const std::filesystem::path& directory) {
    const auto sid = mouse::CurrentUserSid();
    Check(sid.rfind(L"S-1-", 0) == 0, "Startup user is not a Windows SID");
    const auto path = directory / L"鼠标 & 统计's" / L"MouseClickStats.exe";
    const auto xml = mouse::ElevatedStartupTaskXml(path, sid);
    Check(xml.find(L"鼠标 &amp; 统计&apos;s") != std::wstring::npos, "Startup executable path was not XML escaped");
    Check(xml.find(L"<UserId>" + sid + L"</UserId>") != std::wstring::npos && xml.find(sid, xml.find(sid) + sid.size()) != std::wstring::npos, "Startup trigger and principal do not target the same user");
    Check(xml.find(L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel>") != std::wstring::npos, "Startup does not request an interactive elevated user token");
    Check(xml.find(L"<Arguments>--background --take-over</Arguments>") != std::wstring::npos && xml.find(L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>") != std::wstring::npos, "Startup would open a dashboard or stop long-running capture");
    Check(xml.find(L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>") != std::wstring::npos && xml.find(L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>") != std::wstring::npos, "Startup is disabled or terminated on battery");
    Check(xml.find(L"Password") == std::wstring::npos && xml.find(L"BootTrigger") == std::wstring::npos, "Startup requires stored credentials or a system boot trigger");
    bool rejected = false;
    try { mouse::ElevatedStartupTaskXml(L"relative.exe", sid); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Relative startup executable accepted");
    rejected = false;
    try { mouse::ConfigureStartupMode(mouse::StartupMode::Disabled, L"relative.exe"); } catch (...) { rejected = true; }
    Check(rejected, "Invalid startup path reached system configuration");
    rejected = false;
    try { mouse::ChangeStartupMode(nullptr, static_cast<mouse::StartupMode>(99), path); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Invalid startup selection reached system configuration");
    // Windows validates the real schema; TASK_VALIDATE_ONLY never registers or runs a task.
    mouse::ValidateStartupTaskForTesting(path);
    Check(true, "Windows startup task XML validation failed");
}
void TestActivationRequest() {
    auto window = CreateWindowExW(0, L"STATIC", L"MouseClickStats activation test", WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) throw std::runtime_error("Cannot create activation request test window");
    const bool queued = mouse::RequestDashboardActivation(window, true);
    MSG request{}; const bool received = PeekMessageW(&request, window, mouse::kShowDashboardMessage, mouse::kShowDashboardMessage, PM_REMOVE) != FALSE;
    const bool hidden = !IsWindowVisible(window);
    const bool topmost = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    DestroyWindow(window);
    Check(!mouse::RequestDashboardActivation(nullptr), "Invalid activation target accepted");
    Check(queued && received && request.wParam == 1 && request.lParam == 0, "Dashboard activation/settings intent was not delivered");
    Check(hidden && !topmost, "Caller changed window visibility or permanently set topmost");
}
void TestTaskbarAttribution() {
    const auto realTaskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    // Hidden local windows exercise the same class/ancestor lookup without desktop input.
    struct Window {
        const wchar_t* name; HWND handle{}; ATOM atom{};
        explicit Window(const wchar_t* className) : name(className) {
            WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = name;
            atom = RegisterClassW(&wc);
            if (!atom) throw std::runtime_error("Cannot register attribution test window");
            handle = CreateWindowExW(0, name, L"", WS_OVERLAPPED, 0, 0, 1, 1,
                                     nullptr, nullptr, wc.hInstance, nullptr);
            if (!handle) { UnregisterClassW(name, wc.hInstance); throw std::runtime_error("Cannot create attribution test window"); }
        }
        ~Window() { DestroyWindow(handle); UnregisterClassW(name, GetModuleHandleW(nullptr)); }
    } app(L"MouseClickStats.TestApp.v1"), taskbar(L"Shell_TrayWnd"), secondary(L"Shell_SecondaryTrayWnd"), explorer(L"CabinetWClass");
    const auto child = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1,
                                      taskbar.handle, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(child != nullptr, "Cannot create taskbar child test window");
    mouse::ForegroundTracker tracker;
    const auto expected = mouse::ResolveProcessIdentity(GetCurrentProcessId()).key;
    Check(tracker.ForInput(app.handle, app.handle).key == expected, "Ordinary application attribution changed");
    Check(tracker.ForInput(app.handle, child).key == mouse::kTaskbarAppKey, "Taskbar child counted against the old foreground app");
    Check(tracker.ForInput(nullptr, child).key == mouse::kTaskbarAppKey, "Null foreground during minimization became unknown");
    Check(tracker.ForInput(app.handle, secondary.handle).key == mouse::kTaskbarAppKey, "Secondary taskbar was not classified");
    Check(tracker.ForInput(app.handle, taskbar.handle).name == u8"Windows 任务栏", "Taskbar display name missing");
    Check(tracker.ForInput(app.handle, explorer.handle).key == expected, "Explorer application was mistaken for the taskbar");
    Check(tracker.ForInput(app.handle, nullptr).key == expected, "Missing hit window lost known foreground identity");
    Check(tracker.ForInput(nullptr, app.handle).key == expected, "Activation click with a null foreground became unknown");
    Check(tracker.ForInput(nullptr, child, false).key == mouse::kTaskbarAppKey, "Taskbar wheel lost its system category");
    Check(tracker.ForInput(nullptr, app.handle, false).key == expected, "Wheel with a null foreground lost its hit-window fallback");
    Check(tracker.ForInput(nullptr, nullptr).key == "unknown", "Missing foreground and hit window invented an application");
    const auto appChild = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1, app.handle, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(appChild && tracker.ForInput(nullptr, appChild).key == expected, "Clicked application child did not resolve to its root");
    const auto previousCapture = SetCapture(app.handle);
    const bool capturedApp = GetCapture() == app.handle && tracker.ForInput(app.handle, child).key == expected;
    ReleaseCapture(); if (previousCapture) SetCapture(previousCapture);
    Check(capturedApp, "Hovered taskbar overrode the application's mouse capture");
    Check(tracker.ForInput(app.handle, app.handle).key == expected, "Taskbar identity leaked into subsequent app input");
    if (realTaskbar)
        Check(tracker.ForInput(nullptr, realTaskbar).key == mouse::kTaskbarAppKey, "Real Windows taskbar was not classified");
}
void TestCapture() {
    Check(mouse::ProcessIntegrity() >= SECURITY_MANDATORY_LOW_RID, "Cannot determine own integrity level");
    Check(mouse::SameProcessUser(GetCurrentProcessId()), "Own account identity mismatch");
    Check(!mouse::SameProcessUser(0) && mouse::ProcessIntegrity(0) == 0, "Invalid process privileges accepted");
    const auto pathIdentity = mouse::ResolveProcessIdentity(GetCurrentProcessId());
    const auto nameIdentity = mouse::ResolveProcessIdentity(GetCurrentProcessId(), true);
    Check(pathIdentity.key != "unknown" && pathIdentity.name == nameIdentity.name, "Executable fallback lost process name");
    Check(nameIdentity.key.rfind("process-name:", 0) == 0, "Executable fallback pretended to be a full path");
    Check(mouse::ResolveProcessIdentity(0, true).key == "unknown", "Invalid PID resolved");
    const auto event = [](bool raw, uint32_t tick, uint16_t flags = mouse::kLeftDown) {
        return mouse::InputEvent{tick, flags, 0, "2026-11-01", "game", "Game", raw};
    };
    mouse::InputMerger merger;
    Check(merger.Push(event(false, 100), 100).empty(), "Hook committed before raw twin could arrive");
    auto result = merger.Push(event(true, 100), 101);
    Check(result.size() == 1 && result[0].raw, "Raw twin did not win");
    Check(merger.Drain(180).empty(), "Hook/raw pair counted twice");
    Check(merger.Push(event(true, 200), 200).size() == 1, "Raw input delayed");
    Check(merger.Push(event(false, 200), 201).empty(), "Reversed pair counted twice");
    merger.Push(event(false, 300), 300);
    result = merger.Drain(375);
    Check(result.size() == 1 && !result[0].raw, "Compatibility-only click lost");
    Check(merger.Push(event(true, 300), 400).empty(), "Late raw input duplicated a committed fallback");
    merger.Push(event(false, 500), 500); merger.Push(event(false, 500), 500);
    Check(merger.Push(event(true, 500), 501).size() == 1 && merger.Push(event(true, 500), 501).size() == 1, "Fast clicks sharing one timestamp were debounced");
    Check(merger.Drain(580).empty(), "Fast click pair counted twice");
    merger.Push(event(false, 600, mouse::kLeftDown | mouse::kRightDown), 600);
    Check(merger.Push(event(true, 600, mouse::kLeftDown | mouse::kRightDown), 601).size() == 2, "Simultaneous buttons lost");
    mouse::InputMerger wrap;
    wrap.Push(event(false, 0xfffffff8), 0xfffffff8);
    Check(wrap.Push(event(true, 2), 2).size() == 1 && wrap.Drain(100).empty(), "DWORD timestamp wrap broke deduplication");
    wrap.Push(event(false, 200), 200);
    Check(wrap.Drain(201, true).size() == 1 && !wrap.Pending(), "Exit/pause did not drain pending clicks");
}
void TestRecentActivity() {
    mouse::RecentActivity recent;
    recent.Record(1000, mouse::kLeftDown | mouse::kRightDown);
    recent.Record(1001, mouse::kWheel | mouse::kHWheel);
    recent.Record(1799999, mouse::kX1Down);
    auto value = recent.Snapshot(1799999);
    Check(value.halfHour == 3 && value.hour == 3, "Recent activity counted scroll or lost simultaneous buttons");
    value = recent.Snapshot(1800000);
    Check(value.halfHour == 1 && value.hour == 3, "30-minute boundary retained an expired minute");
    recent.Record(1800000, mouse::kMiddleDown | mouse::kWheel);
    value = recent.Snapshot(3600000);
    Check(value.halfHour == 0 && value.hour == 2, "Hour boundary or inactive half-hour is wrong");
    value = recent.Snapshot(5400000);
    Check(!value.halfHour && !value.hour && value.observedMs == 3600000, "Idle/sleep did not expire recent activity");
    mouse::RecentActivity clearing(1000);
    clearing.Record(2000, mouse::kLeftDown | mouse::kX2Down);
    const auto checkpoint = clearing;
    clearing.Record(3000, mouse::kRightDown);
    Check(clearing.Snapshot(4000).hour == 3, "Pending/failed deletion lost recent counts");
    clearing.Discard(checkpoint, 2500);
    value = clearing.Snapshot(4000);
    Check(value.hour == 1 && value.observedMs == 1500, "Successful clear removed post-clear input or retained old input");
    Check(mouse::ActivityMood({6000,10000,299999}) == mouse::PulseMood::Warming, "Very short warmup invented a sustained APM claim");
    Check(mouse::ActivityMood({0,0,3600000}) == mouse::PulseMood::Quiet, "Idle activity got a high-speed headline");
    Check(mouse::ActivityMood({1799,1799,3600000}) == mouse::PulseMood::Casual, "Casual activity threshold incorrect");
    Check(mouse::ActivityMood({1800,1800,3600000}) == mouse::PulseMood::Busy, "Busy threshold did not trigger at 1800");
    Check(mouse::ActivityMood({3600,3600,3600000}) == mouse::PulseMood::Turbo, "Turbo threshold did not trigger at 3600");
    Check(mouse::ActivityMood({6000,9999,3600000}) == mouse::PulseMood::Starcraft, "Starcraft threshold did not trigger at 6000");
    Check(mouse::ActivityMood({100,10000,3600000}) == mouse::PulseMood::Marathon, "One-hour milestone did not trigger independently");
}
void TestCollectorLifecycle() {
    mouse::CollectorWatch watch;
    Check(!watch.ShouldClose(false, false, 0) && !watch.ShouldClose(false, false, 9000), "Startup without a collector closed the dashboard");
    Check(!watch.ShouldClose(true, false, 10000), "Connected collector closed the dashboard");
    Check(!watch.ShouldClose(false, false, 11000) && !watch.ShouldClose(false, false, 18999), "Short disconnect closed the dashboard");
    Check(watch.ShouldClose(false, false, 19000), "Exited collector left its dashboard open");
    Check(!watch.ShouldClose(true, false, 20000) && !watch.ShouldClose(false, false, 21000), "Reconnect did not reset the shutdown deadline");
    Check(!watch.ShouldClose(false, true, 30000) && !watch.ShouldClose(false, true, 60000), "Elevation handoff closed the dashboard");
    Check(!watch.ShouldClose(false, false, 61000) && !watch.ShouldClose(true, false, 62000), "Dashboard did not survive collector handoff");
}
void TestCounter() {
    mouse::Counter counter;
    Check(!counter.Record("2026-10-31", "game", "Game", 0), "Movement counted");
    Check(!counter.Record("2026-10-31", "game", "Game", 0x2 | 0x8), "Release counted");
    counter.Record("2026-10-31", "game", "Game", mouse::kLeftDown | mouse::kRightDown);
    counter.Record("2026-10-31", "game", "Game", mouse::kLeftDown);
    counter.Record("2026-10-31", "game", "Game", mouse::kMiddleDown | mouse::kX1Down | mouse::kX2Down);
    for (int i = 0; i < 3; ++i) counter.Record("2026-10-31", "game", "Game", mouse::kWheel, 40);
    counter.Record("2026-10-31", "game", "Game", mouse::kWheel, -120);
    counter.Record("2026-10-31", "game", "Game", mouse::kHWheel, -32768);
    counter.SetPaused(true);
    Check(!counter.Record("2026-10-31", "game", "Game", mouse::kLeftDown), "Pause failed");
    counter.SetPaused(false);
    counter.Record("2026-11-01", "game", "Game", mouse::kLeftDown);
    counter.Record("2026-11-01", "browser", "Browser", mouse::kRightDown);
    auto data = counter.TakePending();
    Check(data.size() == 3, "Date/application separation failed");
    const auto& first = data.at({"2026-10-31", "game"}).counts;
    Check(first.TotalClicks() == 6, "Simultaneous buttons/double click incorrect");
    Check(first.clicks[0] == 2 && first.clicks[4] == 1, "Per-button count incorrect");
    Check(first.wheelUp == 120 && first.wheelDown == 120, "Partial wheel deltas lost");
    Check(first.wheelLeft == 32768 && first.wheelRight == 0, "Signed wheel conversion failed");
    Check(counter.TakePending().empty(), "Pending counts were not drained");
    mouse::Counts large;
    large.clicks[0] = (std::numeric_limits<int64_t>::max)();
    mouse::Counts one; one.clicks[0] = 1; large.Add(one);
    Check(large.clicks[0] == (std::numeric_limits<int64_t>::max)(), "Counter overflow");
    Check(mouse::ShiftDate("2024-03-01", -1) == "2024-02-29", "Leap day incorrect");
    Check(mouse::ShiftDate("2026-12-31", 1) == "2027-01-01", "Year transition incorrect");
    const std::wstring unicode = L"测试鼠标 🎮";
    Check(mouse::Wide(mouse::Utf8(unicode)) == unicode, "Unicode conversion failed");
    Check(mouse::CsvCell("=SUM(1)") == "\"'=SUM(1)\"", "CSV formula protection failed");
}
void TestClickSpeed() {
    using Test = mouse::ClickSpeedTest;
    Test test;
    for (int seconds : {1, 3}) for (int button : {0, 1}) {
        test.Reset(seconds, button);
        Check(!test.Press(1 - button, 500) && test.Status() == Test::State::Ready && !test.Clicks(), "Wrong button started click test");
        Check(test.Press(button, 1000) && test.Clicks() == 1, "First click did not start and count");
        test.Press(button, 1000); // Separate presses at the same timestamp, including double clicks.
        test.Press(1 - button, 2000);
        const auto deadline = 1000 + seconds * 1000000ULL;
        Check(test.Press(button, deadline - 1) && test.Clicks() == 3, "Valid last click or double click was lost");
        Check(!test.Press(button, deadline) && test.Status() == Test::State::Finished && test.Clicks() == 3, "Deadline click was incorrectly accepted");
        Check(test.RemainingUs(deadline) == 0 && test.Cps() == 3.0 / seconds && test.Apm() == 180.0 / seconds, "Click speed or APM duration conversion incorrect");
        Check(!test.Press(button, deadline + 1) && test.Clicks() == 3, "Completed click test restarted itself");
    }
    test.Reset(1, 0); test.Press(0, 0); test.Advance(1000000);
    Check(test.Status() == Test::State::Finished && test.Clicks() == 1, "Idle deadline failed to complete or long hold repeated");
    test.Reset(3, 1); test.Press(1, 100); test.Reset(1, 0);
    Check(test.Status() == Test::State::Ready && !test.Clicks() && test.RemainingUs(500) == 1000000, "Reset retained previous test state");
}
void TestLifetime(const std::filesystem::path& directory) {
    mouse::SqliteStore store(directory / L"lifetime.db");
    Check(store.ReadLifetime().counts.Empty() && store.ReadLifetime().firstDate.empty(), "Empty lifetime statistics incorrect");
    mouse::Batch batch;
    batch[{"2001-01-01", "old"}] = {"Old", {}};
    batch[{"2001-01-01", "old"}].counts.clicks = {10, 20, 30, 40, 50};
    batch[{"2026-11-01", "new"}] = {"New", {}};
    batch[{"2026-11-01", "new"}].counts.clicks = {1, 2, 3, 4, 5};
    batch[{"2026-11-01", "new"}].counts.wheelUp = 120;
    store.Save(batch);
    auto value = store.ReadLifetime();
    Check(value.firstDate == "2001-01-01" && value.counts.clicks == std::array<int64_t,5>{11,22,33,44,55}, "Lifetime lost older dates, applications or button totals");
    Check(value.counts.TotalClicks() == 165 && !value.counts.wheelUp, "Lifetime click total included scrolling");
    batch.clear(); batch[{"2026-11-02", "max"}].name = "Max";
    batch.begin()->second.counts.clicks[0] = (std::numeric_limits<int64_t>::max)();
    store.Save(batch); value = store.ReadLifetime();
    Check(value.counts.clicks[0] == (std::numeric_limits<int64_t>::max)() && value.counts.TotalClicks() == (std::numeric_limits<int64_t>::max)(), "Lifetime overflow lost saturation");
    store.ClearAll();
    Check(store.ReadLifetime().firstDate.empty() && store.ReadLifetime().counts.Empty(), "Lifetime survived clearing statistics");
}
void TestDashboardModel() {
    std::vector<mouse::StoredRow> rows;
    mouse::Counts a; a.clicks = {10, 2, 1, 0, 0}; a.wheelUp = 40;
    mouse::Counts b; b.clicks[0] = 5;
    rows.push_back({{"2026-11-01", "game"}, "Game", a});
    rows.push_back({{"2026-11-01", "browser"}, "Browser", b});
    rows.push_back({{"2026-10-31", "game"}, "Game", b});
    rows.push_back({{"2026-10-30", "old-app"}, "Old", b});
    auto model = mouse::BuildDashboardModel(rows, "2026-11-01", 7);
    Check(model.selected.TotalClicks() == 18, "Dashboard selected-day sum incorrect");
    Check(model.previous.TotalClicks() == 5 && model.period.TotalClicks() == 28, "Dashboard comparison/range totals incorrect");
    Check(model.apps.size() == 2 && model.apps[0].key == "game", "Inactive application leaked into daily ranking");
    const auto* game = model.FindApp("game");
    Check(game && game->period.TotalClicks() == 18 && game->previous.TotalClicks() == 5, "Application inspector totals incorrect");
    Check(game && game->days.back().wheelUp == 40, "Application trend lost precise wheel data");
    Check(model.dates.front() == "2026-10-26" && model.peakDay == 6, "Trend calendar/peak incorrect");
    auto empty = mouse::BuildDashboardModel(rows, "2020-01-01", 90);
    Check(empty.selected.Empty() && empty.apps.empty() && empty.days.size() == 90, "Empty historical view incorrect");
}
void TestStorage(const std::filesystem::path& directory) {
    auto path = directory / L"statistics.db";
    mouse::SqliteStore store(path);
    mouse::Counter counter;
    counter.Record("2026-10-31", u8"c:\\游戏\\a.exe", u8"游戏,\"A\".exe", mouse::kLeftDown | mouse::kX2Down);
    counter.Record("2026-11-01", "browser", "Browser", mouse::kRightDown | mouse::kWheel, 40);
    auto batch = counter.TakePending();
    store.Save(batch);
    store.Save(batch);
    mouse::SqliteStore reader(path, true);
    auto rows = reader.Read("2026-10-31", "2026-10-31");
    Check(rows.size() == 1 && rows[0].counts.TotalClicks() == 4, "Additive persistence/date filter failed");
    Check(rows[0].name == u8"游戏,\"A\".exe", "Unicode/quoted application name damaged");
    Check(reader.Read("2026-10-01", "2026-11-30").size() == 2, "History read failed");
    Check(reader.Read("2025-01-01", "2025-12-31").empty(), "Empty date range failed");
    store.ExecuteForTesting("CREATE TRIGGER reject_browser BEFORE UPDATE ON daily_stats WHEN NEW.app_key='browser' BEGIN SELECT RAISE(ABORT,'test rollback'); END;");
    bool failed = false;
    try { store.Save(batch); } catch (...) { failed = true; }
    Check(failed, "Expected transaction failure did not occur");
    Check(reader.Read("2026-10-31", "2026-10-31")[0].counts.TotalClicks() == 4, "Partial transaction committed");
    store.ExecuteForTesting("DROP TRIGGER reject_browser");
    store.Save(batch);
    Check(reader.Read("2026-10-31", "2026-10-31")[0].counts.TotalClicks() == 6, "Retry double-counted rolled-back transaction");
    store.ExportCsv(directory / L"export.csv", "2026-10-01", "2026-11-30");
    std::ifstream csv(directory / L"export.csv", std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(csv)), {});
    Check(contents.substr(0, 3) == "\xEF\xBB\xBF", "CSV UTF-8 BOM missing");
    Check(contents.find(u8"游戏,\"\"A\"\".exe") != std::string::npos, "CSV escaping failed");
    auto futurePath = directory / L"future.db";
    { mouse::SqliteStore future(futurePath); future.ExecuteForTesting("PRAGMA user_version=999"); }
    failed = false;
    try { mouse::SqliteStore future(futurePath); } catch (...) { failed = true; }
    Check(failed, "Future schema accepted");
}
void TestWorker(const std::filesystem::path& directory) {
    const auto path = directory / L"worker.db";
    mouse::Persistence worker(path);
    mouse::Counter counter;
    for (int i = 0; i < 50; ++i) {
        counter.Record("2026-11-01", "game", "Game", mouse::kLeftDown);
        worker.Submit(counter.TakePending());
    }
    Check(worker.Stop(), "Worker shutdown flush failed");
    mouse::SqliteStore reader(path, true);
    const auto rows = reader.Read("2026-11-01", "2026-11-01");
    Check(rows.size() == 1 && rows[0].counts.TotalClicks() == 50, "Worker dropped/duplicated queued batch");
}
void TestWorkerRecovery(const std::filesystem::path& directory) {
    const auto path = directory / L"recovery.db";
    mouse::SqliteStore blocker(path);
    blocker.ExecuteForTesting("BEGIN IMMEDIATE");
    mouse::Persistence worker(path);
    mouse::Counter counter;
    counter.Record("2026-11-01", "game", "Game", mouse::kLeftDown | mouse::kX1Down);
    worker.Submit(counter.TakePending());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker.Failed() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(worker.Failed() && !worker.Error().empty(), "Database lock did not surface a persistence error");
    blocker.ExecuteForTesting("ROLLBACK");
    // Stop wakes the retry delay and must commit all previously failed deltas once.
    Check(worker.Stop(), "Pending data was not recovered after database unlock");
    mouse::SqliteStore reader(path, true);
    const auto rows = reader.Read("2026-11-01", "2026-11-01");
    Check(rows.size() == 1 && rows[0].counts.TotalClicks() == 2, "Recovery lost or duplicated deltas");
}
void TestClearStorage(const std::filesystem::path& directory) {
    const auto path = directory / L"clear-storage.db";
    mouse::SqliteStore store(path);
    mouse::Counter counter;
    counter.Record("2026-10-01", "old-game", "Old game", mouse::kLeftDown);
    counter.Record("2026-11-01", "browser", "Browser", mouse::kRightDown);
    store.Save(counter.TakePending());
    store.ExecuteForTesting("CREATE TRIGGER reject_clear BEFORE DELETE ON applications BEGIN SELECT RAISE(ABORT,'clear rollback test'); END;");
    bool failed = false;
    try { store.ClearAll(); } catch (...) { failed = true; }
    Check(failed && store.Read("2000-01-01", "2100-01-01").size() == 2, "Failed clear did not roll back both tables");
    store.ExecuteForTesting("DROP TRIGGER reject_clear");
    store.ClearAll();
    Check(store.Read("2000-01-01", "2100-01-01").empty(), "Clear left historical statistics");
    // A plain insert would fail if the applications table still retained the old key.
    store.ExecuteForTesting("INSERT INTO applications VALUES('old-game','New session')");
    counter.Record("2026-11-01", "old-game", "New session", mouse::kX1Down);
    store.Save(counter.TakePending());
    Check(store.Read("2026-11-01", "2026-11-01")[0].counts.TotalClicks() == 1, "Counting did not resume after clearing");
    mouse::SqliteStore readOnly(path, true);
    failed = false; try { readOnly.ClearAll(); } catch (...) { failed = true; }
    Check(failed && readOnly.Read("2000-01-01", "2100-01-01").size() == 1, "Read-only connection modified statistics");
}
void TestPortablePeer(const std::filesystem::path& directory) {
    const auto sibling = directory / L"MouseClickStats-9.8.7-portable";
    const auto shared = directory / L"MouseClickStats-9.8.7-small" / L"MouseClickStats.UI.exe";
    std::filesystem::create_directories(sibling);
    Check(mouse::PortableDashboardPeer(shared).empty(), "Incomplete portable folder selected");
    std::ofstream(sibling / L"MouseClickStats.UI.exe") << "test";
    std::ofstream(sibling / L"Microsoft.UI.Xaml.dll") << "test";
    Check(mouse::PortableDashboardPeer(shared).empty(), "Portable folder without resources selected");
    std::ofstream(sibling / L"resources.pri") << "test";
    Check(mouse::PortableDashboardPeer(shared) == sibling / L"MouseClickStats.UI.exe", "Matching local portable bundle not selected");
    Check(mouse::PortableDashboardPeer(directory / L"MouseClickStats-9.8.8-small" / L"MouseClickStats.UI.exe").empty(), "Different-version portable bundle selected");
    Check(mouse::PortableDashboardPeer(sibling / L"MouseClickStats.UI.exe").empty(), "Portable launcher redirected itself");
    const auto runtime = sibling / L"runtime";
    std::filesystem::create_directory(runtime);
    for (const auto* name : {L"MouseClickStats.UI.exe", L"Microsoft.UI.Xaml.dll", L"resources.pri"}) std::ofstream(runtime / name) << "test";
    const auto internal = directory / L"MouseClickStats-9.8.7-small/runtime/MouseClickStats.UI.exe";
    Check(mouse::InstallationDirectory(internal) == directory / L"MouseClickStats-9.8.7-small", "Runtime UI resolved wrong application root");
    Check(mouse::DashboardExecutable(sibling / L"MouseClickStats.exe") == runtime / L"MouseClickStats.UI.exe", "Main launcher did not select runtime UI");
    Check(mouse::PortableDashboardPeer(internal) == runtime / L"MouseClickStats.UI.exe", "Shared runtime UI did not select packaged portable peer");
    Check(mouse::InstallationDirectory(directory / L"runtime/MouseClickStats.exe") == directory / L"runtime", "Root launcher named runtime resolved wrong parent");
}
void TestClearWorker(const std::filesystem::path& directory, bool reject) {
    const auto path = directory / (reject ? L"clear-worker-failure.db" : L"clear-worker.db");
    mouse::SqliteStore store(path);
    mouse::Batch history; history[{"2026-10-01", "old"}] = {"Old", {{7,0,0,0,0}}}; store.Save(history);
    if (reject) store.ExecuteForTesting("CREATE TRIGGER reject_clear BEFORE DELETE ON applications BEGIN SELECT RAISE(ABORT,'worker clear rollback'); END;");
    mouse::Persistence worker(path);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker.Ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(worker.Ready(), "Clear worker did not initialize");
    // Hold the writer so the clear must order itself with an earlier queued/in-flight save.
    if (!reject) store.ExecuteForTesting("BEGIN IMMEDIATE");
    mouse::Batch before, after;
    before[{"2026-11-01", "before"}] = {"Before", {{5,0,0,0,0}}};
    after[{"2026-11-01", "after"}] = {"After", {{0,2,0,0,0}}};
    worker.Submit(before); const auto ticket = worker.RequestClear(); worker.Submit(after);
    if (!reject) store.ExecuteForTesting("ROLLBACK");
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (worker.Clearing(ticket) == mouse::ClearState::Pending && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(ticket && worker.Clearing(ticket) == (reject ? mouse::ClearState::Failed : mouse::ClearState::Succeeded), "Clear worker result mismatch");
    if (reject) store.ExecuteForTesting("DROP TRIGGER reject_clear");
    Check(worker.Stop(), "Post-clear queued data did not drain");
    const auto rows = store.Read("2000-01-01", "2100-01-01");
    int64_t total = 0; for (const auto& row : rows) total += row.counts.TotalClicks();
    Check(reject ? (rows.size() == 3 && total == 14) : (rows.size() == 1 && rows[0].key.app == "after" && total == 2),
          reject ? "Failed clear lost or duplicated buffered statistics" : "Old queued statistics reappeared after clearing");
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--activate-dashboard-fixture") {
        try {
            const auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::stoull(argv[2])));
            if (!IsWindow(window) || !GetPropW(window, L"MouseClickStats.Verification")) return 2;
            return mouse::RequestDashboardActivation(window, true) ? 0 : 3;
        } catch (...) { return 2; }
    }
    if (argc != 1) return 2;
    auto directory = std::filesystem::current_path() / ("test-data-" + std::to_string(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(directory);
        TestCounter(); TestCapture(); TestTaskbarAttribution(); TestRecentActivity(); TestCollectorLifecycle(); TestClickSpeed(); TestLifetime(directory); TestDashboardModel(); TestStorage(directory); TestWorker(directory); TestWorkerRecovery(directory);
        TestStartupConfiguration(directory); TestActivationRequest();
        TestClearStorage(directory); TestClearWorker(directory, false); TestClearWorker(directory, true); TestPortablePeer(directory);
        const auto resolved = std::filesystem::weakly_canonical(directory);
        if (resolved.parent_path() != std::filesystem::canonical(std::filesystem::current_path()) ||
            resolved.filename() != directory.filename()) throw std::runtime_error("Unsafe test cleanup path");
        std::filesystem::remove_all(resolved);
        std::cout << "PASS: " << checks << " checks (counters, dates, Unicode, SQLite, rollback, CSV, worker shutdown)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\nTest data: " << directory << '\n';
        return 1;
    }
}
