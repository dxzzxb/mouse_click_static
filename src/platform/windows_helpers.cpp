#include "platform/windows_helpers.h"
#include <shellapi.h>
#include <shlobj.h>
#include <stdexcept>
#include <vector>
#include <cwchar>
#include <cstdio>
#include <tlhelp32.h>

namespace mouse {
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid UTF-16 text");
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size);
    return out;
}
std::filesystem::path ExecutablePath() {
    std::vector<wchar_t> path(32768);
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("Cannot resolve executable path");
    return std::wstring(path.data(), length);
}
std::filesystem::path DataDirectory() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value)))
        throw std::runtime_error("Cannot resolve LocalAppData");
    std::filesystem::path result(value);
    CoTaskMemFree(value);
    result /= L"MouseClickStats";
    std::filesystem::create_directories(result);
    return result;
}
std::filesystem::path InstallationDirectory(const std::filesystem::path& executable) {
    auto parent = executable.parent_path();
    if (executable.filename() == L"MouseClickStats.UI.exe" && parent.filename() == L"runtime") return parent.parent_path();
    return parent;
}
std::filesystem::path DashboardExecutable(const std::filesystem::path& executable) {
    const auto root = InstallationDirectory(executable);
    const auto ui = root / L"runtime" / L"MouseClickStats.UI.exe";
    if (std::filesystem::is_regular_file(ui)) return ui;
    const auto legacy = root / L"MouseClickStats.UI.exe";
    return std::filesystem::is_regular_file(legacy) ? legacy : ui;
}
std::filesystem::path CollectorExecutable() { return InstallationDirectory(ExecutablePath()) / L"MouseClickStats.exe"; }
std::filesystem::path PortableDashboardPeer(const std::filesystem::path& executable) {
    const auto directory = InstallationDirectory(executable);
    const auto name = directory.filename().wstring();
    const std::wstring suffix = L"-small";
    if (name.rfind(L"MouseClickStats", 0) != 0 || name.size() <= suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return {};
    const auto peer = directory.parent_path() / (name.substr(0, name.size() - suffix.size()) + L"-portable");
    const auto ui = DashboardExecutable(peer / L"MouseClickStats.exe");
    if (!std::filesystem::is_regular_file(ui) ||
        !std::filesystem::is_regular_file(ui.parent_path() / L"Microsoft.UI.Xaml.dll") ||
        !std::filesystem::is_regular_file(ui.parent_path() / L"resources.pri")) return {};
    return ui;
}
std::filesystem::path DatabasePath() { return DataDirectory() / L"stats.db"; }
std::string LocalDate(int daysAgo) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    // Use noon as a calendar anchor, not UTC seconds, to avoid DST/date-boundary errors.
    local.wHour = 12; local.wMinute = local.wSecond = local.wMilliseconds = 0;
    FILETIME ft{};
    SystemTimeToFileTime(&local, &ft);
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime; value.HighPart = ft.dwHighDateTime;
    value.QuadPart -= static_cast<ULONGLONG>(daysAgo) * 864000000000ULL;
    ft.dwLowDateTime = value.LowPart; ft.dwHighDateTime = value.HighPart;
    FileTimeToSystemTime(&ft, &local);
    char date[11]{};
    std::snprintf(date, sizeof(date), "%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
    return date;
}
std::string ShiftDate(const std::string& date, int offset) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') throw std::runtime_error("Invalid date");
    for (size_t i = 0; i < date.size(); ++i) if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) throw std::runtime_error("Invalid date");
    const auto year = std::stoi(date.substr(0, 4)), month = std::stoi(date.substr(5, 2)), day = std::stoi(date.substr(8, 2));
    SYSTEMTIME time{};
    time.wYear = static_cast<WORD>(year); time.wMonth = static_cast<WORD>(month); time.wDay = static_cast<WORD>(day); time.wHour = 12;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&time, &ft)) throw std::runtime_error("Invalid calendar date");
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime; value.HighPart = ft.dwHighDateTime;
    value.QuadPart += static_cast<int64_t>(offset) * 864000000000LL;
    ft.dwLowDateTime = value.LowPart; ft.dwHighDateTime = value.HighPart;
    if (!FileTimeToSystemTime(&ft, &time)) throw std::runtime_error("Date outside supported range");
    char out[11]{};
    std::snprintf(out, sizeof(out), "%04u-%02u-%02u", time.wYear, time.wMonth, time.wDay);
    return out;
}
HWND CollectorWindow() { return FindWindowW(kCollectorClass, nullptr); }
LRESULT CollectorCommand(UINT message, WPARAM parameter) {
    DWORD_PTR result = 0;
    auto window = CollectorWindow();
    if (!window || !SendMessageTimeoutW(window, message, parameter, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result)) return 0;
    return static_cast<LRESULT>(result);
}
void Launch(const std::filesystem::path& executable, const std::wstring& arguments, bool foreground) {
    // Explicit application name and quoted argument prevent space/path ambiguity.
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    if (!arguments.empty()) command += L" " + arguments;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | (foreground ? CREATE_SUSPENDED : 0), nullptr,
                        executable.parent_path().c_str(), &startup, &process))
        throw std::runtime_error("Cannot launch application (Win32 " + std::to_string(GetLastError()) + ")");
    if (foreground) {
        // Grant before the child's first window can activate, avoiding a startup race.
        AllowSetForegroundWindow(process.dwProcessId);
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            TerminateProcess(process.hProcess, 1); CloseHandle(process.hThread); CloseHandle(process.hProcess);
            throw std::runtime_error("Cannot resume dashboard process");
        }
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
AppIdentity ResolveProcessIdentity(DWORD pid, bool nameOnly) {
    if (!pid) return {};
    if (!nameOnly) if (auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        const auto resolved = QueryFullProcessImageNameW(process, 0, path.data(), &length);
        CloseHandle(process);
        if (resolved) {
            path.resize(length);
            auto name = Utf8(std::filesystem::path(path).filename().wstring());
            CharLowerBuffW(path.data(), length);
            return {Utf8(path), std::move(name)};
        }
    }
    auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    std::wstring name;
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ProcessID == pid) { name = entry.szExeFile; break; }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (name.empty()) return {};
    const auto display = Utf8(name);
    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    return {"process-name:" + Utf8(name), display};
}
const AppIdentity& ForegroundTracker::Current(bool force) {
    return ForWindow(GetForegroundWindow(), force);
}
const AppIdentity& ForegroundTracker::ForInput(HWND foreground, HWND hitWindow, bool buttonDown) {
    // Mouse capture wins over the hovered window (dragging, menus and games).
    GUITHREADINFO thread{}; thread.cbSize = sizeof(thread);
    const auto foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    if (foregroundThread && GetGUIThreadInfo(foregroundThread, &thread) && thread.hwndCapture)
        return ForWindow(thread.hwndCapture);
    const auto root = hitWindow ? GetAncestor(hitWindow, GA_ROOT) : nullptr;
    wchar_t className[128]{};
    if (root && GetClassNameW(root, className, ARRAYSIZE(className)) &&
        (std::wcscmp(className, L"Shell_TrayWnd") == 0 ||
         std::wcscmp(className, L"Shell_SecondaryTrayWnd") == 0)) {
        static const AppIdentity taskbar{kTaskbarAppKey, u8"Windows 任务栏"};
        return taskbar;
    }
    // A button press can activate another window before GetForegroundWindow catches up,
    // or while it temporarily returns NULL. Resolve the hit root in that case.
    if (buttonDown && root && ForWindow(root).key != "unknown") return identity_;
    if (ForWindow(foreground).key != "unknown") return identity_;
    if (root) return ForWindow(root);
    return identity_;
}
const AppIdentity& ForegroundTracker::ForWindow(HWND foreground, bool force) {
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    // Retry unknown identities, including processes that were still starting on activation.
    if (!force && foreground == window_ && pid == pid_ &&
        (identity_.key != "unknown" || GetTickCount64() - resolvedAt_ < 3000)) return identity_;
    window_ = foreground; pid_ = pid; resolvedAt_ = GetTickCount64();
    identity_ = ResolveProcessIdentity(pid);
    return identity_;
}
std::string CsvCell(const std::string& value) {
    std::string result = "\"";
    // CSV may be opened in Excel; prevent executable formula cells from application names.
    if (!value.empty() && (value[0] == '=' || value[0] == '+' || value[0] == '-' || value[0] == '@' || value[0] == '\t' || value[0] == '\r')) result += '\'';
    for (char c : value) { result += c; if (c == '"') result += '"'; }
    return result + '"';
}
} // namespace mouse

