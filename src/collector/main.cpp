#include "collector/persistence.h"
#include "platform/windows_helpers.h"
#include "storage/sqlite_store.h"
#include "core/input_merger.h"
#include "core/recent_activity.h"
#include "collector/mouse_hook.h"
#include <shellapi.h>
#include <windowsx.h>
#include <memory>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <optional>

namespace {
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kHookMessage = WM_APP + 2;
constexpr UINT kFlushTimer = 1, kSmokeTimer = 2, kLaunchTimer = 3;
constexpr UINT kMergeTimer = 4;
constexpr UINT kActivateTimer = 5;
constexpr UINT kTrayRetryTimer = 6;
enum : UINT { Open = 100, Pause, Startup, Folder, Quit };
class Collector;
Collector* instance = nullptr;
std::string Json(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
        else out << c;
    }
    out << '"'; return out.str();
}

class Collector {
public:
    Collector(std::filesystem::path database, bool smoke, bool show, bool compatibility)
        : persistence_(std::move(database)), smoke_(smoke), show_(show), compatibility_(compatibility) {}
    int Run(HINSTANCE module) {
        module_ = module;
        instance = this;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = WindowProc; wc.hInstance = module;
        wc.lpszClassName = smoke_ ? L"MouseClickStats.SmokeTest.v1" : mouse::kCollectorClass;
        wc.hIcon = LoadIconW(module, MAKEINTRESOURCEW(1));
        if (!RegisterClassExW(&wc)) throw std::runtime_error("Cannot register collector window");
        window_ = CreateWindowExW(0, wc.lpszClassName, L"Mouse Click Stats Collector", WS_OVERLAPPED,
                                  0, 0, 0, 0, nullptr, nullptr, module, this);
        if (!window_) throw std::runtime_error("Cannot create collector window");
        // Fixed pointer-free commands, with no arbitrary paths or payloads crossing UIPI.
        for (auto message : {mouse::kFlushMessage, mouse::kStatusMessage, mouse::kPauseMessage,
             mouse::kOpenMessage, mouse::kExitMessage, mouse::kReadyMessage, mouse::kDiagnoseMessage,
             mouse::kClearMessage, mouse::kClearStateMessage, mouse::kRecent30Message,
             mouse::kRecent60Message, mouse::kRecentAgeMessage})
            if (!ChangeWindowMessageFilterEx(window_, message, MSGFLT_ALLOW, nullptr))
                throw std::runtime_error("Cannot configure collector communication");
        RAWINPUTDEVICE device{0x01, 0x02, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window_};
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) throw std::runtime_error("Raw Input registration failed");
        if (!smoke_ && compatibility_) hook_ = std::make_unique<mouse::MouseHook>(window_, kHookMessage);
        ObserveForeground();
        eventHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                      ForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!eventHook_) throw std::runtime_error("Foreground event registration failed");
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        if (!smoke_) AddTray();
        if (!SetTimer(window_, kFlushTimer, 30000, nullptr)) throw std::runtime_error("Cannot start persistence timer");
        if (smoke_) SetTimer(window_, kSmokeTimer, 1200, nullptr);
        if (show_) SetTimer(window_, kLaunchTimer, 100, nullptr);
        MSG message{};
        int status;
        while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (status == -1) throw std::runtime_error("Collector message loop failed");
        return persistence_.Failed() ? 2 : 0;
    }
    ~Collector() {
        if (hook_) hook_->Stop();
        if (eventHook_) UnhookWinEvent(eventHook_);
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        instance = nullptr;
    }
private:
    static void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
        if (instance) {
            try { instance->ObserveForeground(); } catch (...) { /* Never unwind through Win32. */ }
        }
    }
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Collector*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Collector*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
        try { return self->OnMessage(message, wParam, lParam); }
        catch (const std::exception& error) {
            // Input callbacks cannot let C++ exceptions cross the system ABI.
            self->captureError_ = true;
            if (message != WM_INPUT) MessageBoxW(hwnd, mouse::Wide(error.what()).c_str(), L"Mouse Click Stats", MB_OK | MB_ICONERROR);
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }
    void Flush() { persistence_.Submit(counter_.TakePending()); }
    void ObserveForeground() {
        const auto& app = tracker_.Current(true);
        DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid && mouse::ProcessIntegrity(pid) > integrity_) higherForeground_ = app.name;
    }
    void Commit(const std::vector<mouse::InputEvent>& events) {
        for (const auto& event : events) if (!counter_.Paused()) {
            counter_.Record(event.date, event.app, event.name, event.flags, event.wheel);
            recent_.Record(GetTickCount64(), event.flags);
            merged_.Record("", event.app, event.name, event.flags, event.wheel);
            if (!event.raw) fallback_.Record("", event.app, event.name, event.flags, event.wheel);
        }
    }
    void HookInput() {
        mouse::HookInput input;
        while (hook_ && hook_->Pop(input)) {
            const auto& app = tracker_.ForInput(input.foreground, WindowFromPoint(input.position));
            hookCounts_.Record("", app.key, app.name, input.flags, input.wheel);
            if (!counter_.Paused()) Commit(merger_.Push({input.timestamp, input.flags, input.wheel,
                mouse::LocalDate(), app.key, app.name, false}, GetTickCount()));
        }
        // A short timer runs only after button events, never for mouse movement or while idle.
        if (merger_.Pending()) SetTimer(window_, kMergeTimer, 75, nullptr);
    }
    void Input(HRAWINPUT handle) {
        const auto timestamp = static_cast<DWORD>(GetMessageTime());
        ++rawPackets_;
        RAWINPUT input{};
        UINT size = sizeof(input);
        const auto read = GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
        if (read == static_cast<UINT>(-1)) { ++rawErrors_; rawErrorCode_ = GetLastError(); return; }
        if (read < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) || input.header.dwType != RIM_TYPEMOUSE) { ++invalidRaw_; return; }
        const auto& raw = input.data.mouse;
        if (!(raw.usButtonFlags & mouse::kRelevantFlags)) return;
        // Use the message's position rather than a cursor that may already have moved.
        // GET_X/Y_LPARAM preserve negative coordinates on secondary monitors.
        const auto position = GetMessagePos();
        const POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
        const bool buttonDown = (raw.usButtonFlags & (mouse::kLeftDown | mouse::kRightDown | mouse::kMiddleDown | mouse::kX1Down | mouse::kX2Down)) != 0;
        const auto& app = tracker_.ForInput(GetForegroundWindow(), WindowFromPoint(point), buttonDown);
        rawCounts_.Record("", app.key, app.name, raw.usButtonFlags, static_cast<int16_t>(raw.usButtonData));
        if (!counter_.Paused()) Commit(merger_.Push({timestamp, raw.usButtonFlags,
            static_cast<int16_t>(raw.usButtonData), mouse::LocalDate(), app.key, app.name, true}, GetTickCount()));
    }
    void Diagnose() {
        HookInput(); Commit(merger_.Drain(GetTickCount())); Flush();
        const auto& app = tracker_.Current(true);
        std::ofstream report(mouse::DataDirectory() / L"diagnostics.json", std::ios::trunc);
        report << "{\"version\":\"0.3.5\",\"pid\":" << GetCurrentProcessId()
            << ",\"integrityRid\":" << integrity_ << ",\"lastHigherIntegrityApp\":" << Json(higherForeground_)
            << ",\"compatibilityEnabled\":" << (compatibility_ ? "true" : "false")
            << ",\"uptimeMs\":" << GetTickCount64() - startedAt_ << ",\"paused\":" << (counter_.Paused() ? "true" : "false")
            << ",\"rawPackets\":" << rawPackets_ << ",\"rawReadErrors\":" << rawErrors_ << ",\"rawLastError\":" << rawErrorCode_
            << ",\"invalidRawPackets\":" << invalidRaw_ << ",\"compatibilityHookInstalled\":" << (hook_ && hook_->Active() ? "true" : "false")
            << ",\"hookInstallError\":" << (hook_ ? hook_->Error() : 0) << ",\"hookQueueDropped\":" << (hook_ ? hook_->Dropped() : 0)
            << ",\"foreground\":{\"key\":" << Json(app.key) << ",\"name\":" << Json(app.name) << "}";
        const auto write = [&](const char* label, const mouse::Counter& source) {
            report << ",\"" << label << "\":["; bool comma = false;
            for (const auto& [key, row] : source.Pending()) {
                if (comma) report << ','; comma = true;
                report << "{\"key\":" << Json(key.app) << ",\"name\":" << Json(row.name) << ",\"buttons\":[";
                for (size_t i = 0; i < row.counts.clicks.size(); ++i) { if (i) report << ','; report << row.counts.clicks[i]; }
                report << "],\"clicks\":" << row.counts.TotalClicks() << '}';
            }
            report << ']';
        };
        write("raw", rawCounts_); write("compatibility", hookCounts_); write("counted", merged_); write("fallback", fallback_);
        report << '}';
        if (!report) throw std::runtime_error("Cannot write diagnostics");
    }
    void AddTray() {
        tray_ = {};
        tray_.cbSize = sizeof(tray_); tray_.hWnd = window_; tray_.uID = 1;
        tray_.uCallbackMessage = kTrayMessage;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        tray_.hIcon = LoadIconW(module_, MAKEINTRESOURCEW(1));
        if (!tray_.hIcon) tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        UpdateTip(false);
        if (!Shell_NotifyIconW(NIM_ADD, &tray_) && !Shell_NotifyIconW(NIM_MODIFY, &tray_)) {
            // A logon task may start before Explorer's notification area is ready.
            SetTimer(window_, kTrayRetryTimer, 1000, nullptr);
            return;
        }
        KillTimer(window_, kTrayRetryTimer);
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }
    void FinishRecentClear() {
        if (!recentClearTicket_) return;
        const auto state = persistence_.Clearing(recentClearTicket_);
        if (state == mouse::ClearState::Pending) return;
        if (state == mouse::ClearState::Succeeded && recentCheckpoint_) recent_.Discard(*recentCheckpoint_, recentClearAt_);
        recentCheckpoint_.reset(); recentClearTicket_ = 0;
    }
    void UpdateTip(bool modify = true) {
        const wchar_t* text = persistence_.Failed() || captureError_
            ? L"Mouse Click Stats · 统计异常，请打开查看"
            : counter_.Paused() ? L"Mouse Click Stats · 已暂停" : L"Mouse Click Stats · 正在统计";
        lstrcpynW(tray_.szTip, text, ARRAYSIZE(tray_.szTip));
        if (modify && !smoke_) Shell_NotifyIconW(NIM_MODIFY, &tray_);
    }
    void OpenDashboard(bool settings = false) {
        Flush();
        auto executable = mouse::DashboardExecutable(mouse::ExecutablePath());
        if (!std::filesystem::exists(executable)) {
            MessageBoxW(window_, L"后台统计正在运行。\n\n未找到 runtime 文件夹中的界面程序。\n请完整解压发布包，保留 MouseClickStats.exe 旁的 runtime 文件夹。",
                        L"Mouse Click Stats", MB_OK | MB_ICONINFORMATION);
            return;
        }
        SetForegroundWindow(window_);
        const bool existing = FindWindowW(nullptr, L"Mouse Click Stats") != nullptr;
        const auto foreground = GetForegroundWindow();
        mouse::LaunchDashboard(executable, settings);
        if (!existing) {
            activationForeground_ = foreground; activationUntil_ = GetTickCount64() + 10000; activationSettings_ = settings;
            SetTimer(window_, kActivateTimer, 100, nullptr);
        }
    }
    void Menu() {
        auto menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, Open, L"打开统计面板");
        AppendMenuW(menu, MF_STRING | (counter_.Paused() ? MF_CHECKED : 0), Pause, L"暂停统计");
        AppendMenuW(menu, MF_STRING, Startup, L"登录自启设置…");
        AppendMenuW(menu, MF_STRING, Folder, L"打开数据目录");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Quit, L"退出（停止统计）");
        SetMenuDefaultItem(menu, Open, FALSE);
        POINT point{}; GetCursorPos(&point);
        SetForegroundWindow(window_);
        const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        switch (choice) {
        case Open: OpenDashboard(); break;
        case Pause: counter_.SetPaused(!counter_.Paused()); Flush(); UpdateTip(); break;
        case Startup: OpenDashboard(true); break;
        case Folder: ShellExecuteW(window_, L"open", mouse::DataDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case Quit: DestroyWindow(window_); break;
        }
    }
    LRESULT OnMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreated_ && message == taskbarCreated_) { AddTray(); return 0; }
        switch (message) {
        case WM_INPUT:
            Input(reinterpret_cast<HRAWINPUT>(lParam));
            // Required foreground raw-input cleanup; safe for sink input as well.
            return DefWindowProcW(window_, message, wParam, lParam);
        case WM_INPUT_DEVICE_CHANGE: tracker_.Current(true); return 0;
        case kHookMessage: HookInput(); return 0;
        case mouse::kDiagnoseMessage: Diagnose(); return 1;
        case mouse::kClearMessage:
            HookInput(); Commit(merger_.Drain(GetTickCount(), true)); Flush();
            FinishRecentClear();
            if (!recentClearTicket_) {
                recentCheckpoint_ = recent_; recentClearAt_ = GetTickCount64();
                recentClearTicket_ = persistence_.RequestClear();
            }
            return static_cast<LRESULT>(recentClearTicket_);
        case mouse::kClearStateMessage: FinishRecentClear(); return static_cast<LRESULT>(persistence_.Clearing(static_cast<uint64_t>(wParam)));
        case mouse::kRecent30Message: FinishRecentClear(); return static_cast<LRESULT>(recent_.Snapshot(GetTickCount64()).halfHour);
        case mouse::kRecent60Message: FinishRecentClear(); return static_cast<LRESULT>(recent_.Snapshot(GetTickCount64()).hour);
        case mouse::kRecentAgeMessage: FinishRecentClear(); return static_cast<LRESULT>(recent_.Snapshot(GetTickCount64()).observedMs);
        case mouse::kFlushMessage: Flush(); return 1;
        case mouse::kReadyMessage: return persistence_.Ready() ? 1 : 0;
        case mouse::kStatusMessage:
            return mouse::kStatusRunning | mouse::kStatusRecentSupported | (counter_.Paused() ? mouse::kStatusPaused : 0) |
                   (persistence_.Ready() ? mouse::kStatusClearSupported : 0) |
                   (integrity_ >= SECURITY_MANDATORY_HIGH_RID ? mouse::kStatusElevated : 0) |
                   (!higherForeground_.empty() ? mouse::kStatusHigherForeground : 0) |
                   ((persistence_.Failed() || captureError_) ? mouse::kStatusStorageError : 0);
        case mouse::kPauseMessage:
            HookInput(); Commit(merger_.Drain(GetTickCount(), true));
            counter_.SetPaused(wParam != 0); Flush(); UpdateTip(); return 1;
        case mouse::kOpenMessage: OpenDashboard(); return 1;
        case mouse::kExitMessage: restarting_ = wParam == mouse::kExitForTakeover; DestroyWindow(window_); return 1;
        case WM_TIMER:
            if (wParam == kFlushTimer) { Flush(); UpdateTip(); }
            else if (wParam == kSmokeTimer) { DestroyWindow(window_); }
            else if (wParam == kLaunchTimer && (persistence_.Ready() || persistence_.Failed())) {
                KillTimer(window_, kLaunchTimer); OpenDashboard();
            }
            else if (wParam == kMergeTimer) { HookInput(); Commit(merger_.Drain(GetTickCount())); if (!merger_.Pending()) KillTimer(window_, kMergeTimer); }
            else if (wParam == kActivateTimer) {
                const auto dashboard = FindWindowW(nullptr, L"Mouse Click Stats");
                const auto foreground = GetForegroundWindow();
                if (GetTickCount64() > activationUntil_ || (foreground != activationForeground_ && foreground != dashboard)) KillTimer(window_, kActivateTimer);
                else if (dashboard && IsWindowVisible(dashboard)) {
                    mouse::RequestDashboardActivation(dashboard, activationSettings_); KillTimer(window_, kActivateTimer);
                }
            }
            else if (wParam == kTrayRetryTimer && !smoke_) AddTray();
            return 0;
        case kTrayMessage:
            if (LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT) OpenDashboard();
            else if (LOWORD(lParam) == WM_CONTEXTMENU) Menu();
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND) Flush();
            else if (wParam == PBT_APMRESUMEAUTOMATIC) tracker_.Current(true);
            return TRUE;
        case WM_QUERYENDSESSION: Flush(); return TRUE;
        case WM_ENDSESSION:
            if (wParam) { endingSession_ = true; DestroyWindow(window_); }
            return 0;
        case WM_CLOSE: DestroyWindow(window_); return 0;
        case WM_DESTROY: {
            if (hook_) hook_->Stop();
            HookInput(); Commit(merger_.Drain(GetTickCount(), true));
            KillTimer(window_, kFlushTimer); KillTimer(window_, kSmokeTimer); KillTimer(window_, kLaunchTimer);
            KillTimer(window_, kMergeTimer);
            KillTimer(window_, kActivateTimer);
            KillTimer(window_, kTrayRetryTimer);
            if (eventHook_) { UnhookWinEvent(eventHook_); eventHook_ = nullptr; }
            RAWINPUTDEVICE remove{0x01, 0x02, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&remove, 1, sizeof(remove));
            if (!smoke_) Shell_NotifyIconW(NIM_DELETE, &tray_);
            Flush();
            if (!persistence_.Stop() && !endingSession_ && !smoke_) {
                auto error = L"有统计数据未能保存。请检查磁盘空间和数据目录权限。\n\n" + mouse::Wide(persistence_.Error());
                MessageBoxW(nullptr, error.c_str(), L"Mouse Click Stats · 保存失败", MB_OK | MB_ICONERROR);
            }
            if (!smoke_ && !restarting_) if (auto dashboard = FindWindowW(nullptr, L"Mouse Click Stats"))
                PostMessageW(dashboard, mouse::kCollectorExitingMessage, 0, 0);
            PostQuitMessage(0);
            return 0;
        }
        default: return DefWindowProcW(window_, message, wParam, lParam);
        }
    }
    HINSTANCE module_{};
    HWND window_{};
    HWINEVENTHOOK eventHook_{};
    UINT taskbarCreated_{};
    NOTIFYICONDATAW tray_{};
    mouse::Counter counter_;
    mouse::ForegroundTracker tracker_;
    mouse::Persistence persistence_;
    std::unique_ptr<mouse::MouseHook> hook_;
    mouse::InputMerger merger_;
    mouse::RecentActivity recent_{GetTickCount64()};
    std::optional<mouse::RecentActivity> recentCheckpoint_;
    uint64_t recentClearTicket_ = 0, recentClearAt_ = 0;
    mouse::Counter rawCounts_, hookCounts_, merged_, fallback_;
    ULONGLONG startedAt_ = GetTickCount64(), rawPackets_ = 0, rawErrors_ = 0, invalidRaw_ = 0;
    DWORD rawErrorCode_ = 0;
    DWORD integrity_ = mouse::ProcessIntegrity();
    std::string higherForeground_;
    bool smoke_ = false, show_ = false, endingSession_ = false, captureError_ = false;
    bool compatibility_ = false;
    bool restarting_ = false;
    ULONGLONG activationUntil_ = 0;
    HWND activationForeground_{};
    bool activationSettings_ = false;
};
} // namespace

int WINAPI wWinMain(HINSTANCE module, HINSTANCE, PWSTR, int) {
    HANDLE mutex = nullptr;
    try {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("Cannot parse arguments");
        bool background = false, smoke = false, diagnose = false, stop = false, elevate = false, takeOver = false, compatibility = false;
        bool configureStartup = false;
        mouse::StartupMode startupMode = mouse::StartupMode::Disabled;
        std::wstring startupUser;
        std::filesystem::path database;
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--background") background = true;
            else if (arg == L"--diagnose") diagnose = true;
            else if (arg == L"--stop") stop = true;
            else if (arg == L"--elevate") elevate = true;
            else if (arg == L"--take-over") takeOver = true;
            else if (arg == L"--compatibility") compatibility = true;
            else if (arg == L"--set-startup" && i + 1 < argc) {
                configureStartup = true; const std::wstring mode = argv[++i];
                if (mode == L"elevated") startupMode = mouse::StartupMode::Elevated;
                else if (mode == L"normal") startupMode = mouse::StartupMode::Normal;
                else if (mode != L"off") { LocalFree(argv); return ERROR_INVALID_PARAMETER; }
            }
            else if (arg == L"--startup-user" && i + 1 < argc) startupUser = argv[++i];
            else if (arg == L"--smoke-test" && i + 1 < argc) { smoke = true; background = true; database = argv[++i]; }
            else { LocalFree(argv); throw std::runtime_error("Unsupported command-line argument"); }
        }
        LocalFree(argv);
        if (configureStartup) {
            // This short-lived helper never starts capture or opens the statistics database.
            if (argc != 5 || mouse::ProcessIntegrity() < SECURITY_MANDATORY_HIGH_RID || startupUser != mouse::CurrentUserSid()) return ERROR_ACCESS_DENIED;
            try { mouse::ConfigureStartupMode(startupMode, mouse::ExecutablePath()); return 0; }
            catch (...) { return 1; }
        }
        if (!startupUser.empty()) return ERROR_INVALID_PARAMETER;
        if (diagnose || stop) return mouse::CollectorCommand(diagnose ? mouse::kDiagnoseMessage : mouse::kExitMessage) ? 0 : 3;
        if (elevate) return mouse::RequestElevatedCollector(nullptr, mouse::ExecutablePath()) ? 0 : 4;
        if (takeOver) {
            if (mouse::ProcessIntegrity() < SECURITY_MANDATORY_HIGH_RID) throw std::runtime_error("Takeover requires UAC consent");
            if (auto existing = mouse::CollectorWindow()) {
                DWORD pid = 0; GetWindowThreadProcessId(existing, &pid);
                if (!mouse::SameProcessUser(pid)) throw std::runtime_error("Use the same Windows account for elevation to keep your statistics");
                auto previous = OpenProcess(SYNCHRONIZE, FALSE, pid);
                mouse::CollectorCommand(mouse::kExitMessage, mouse::kExitForTakeover);
                const auto waited = previous ? WaitForSingleObject(previous, 15000) : WAIT_FAILED;
                if (previous) CloseHandle(previous);
                if (waited != WAIT_OBJECT_0) throw std::runtime_error("Previous collector has not completed saving; restart cancelled");
            }
        } else if (!smoke && mouse::CollectorWindow()) {
            if (!background) mouse::LaunchDashboard(mouse::DashboardExecutable(mouse::ExecutablePath()));
            return 0;
        }
        mutex = CreateMutexExW(nullptr, smoke ? L"Local\\MouseClickStats.SmokeTest.v1" : mouse::kCollectorMutex, 0, SYNCHRONIZE);
        if (!mutex) throw std::runtime_error("Cannot create single-instance mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (!background) mouse::LaunchDashboard(mouse::DashboardExecutable(mouse::ExecutablePath()));
            CloseHandle(mutex); return 0;
        }
        if (database.empty()) database = mouse::DatabasePath();
        Collector collector(database, smoke, !background, compatibility);
        auto result = collector.Run(module);
        CloseHandle(mutex);
        return result;
    } catch (const std::exception& error) {
        if (mutex) CloseHandle(mutex);
        MessageBoxW(nullptr, mouse::Wide(error.what()).c_str(), L"Mouse Click Stats · 启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
}

