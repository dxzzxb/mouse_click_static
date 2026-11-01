#pragma once
#include "core/clear_state.h"
#include "platform/startup.h"
#include <windows.h>
#include <filesystem>
#include <string>

namespace mouse {
inline constexpr wchar_t kCollectorClass[] = L"MouseClickStats.Collector.v1";
inline constexpr wchar_t kCollectorMutex[] = L"Local\\MouseClickStats.Collector.v1";
inline constexpr wchar_t kDashboardMutex[] = L"Local\\MouseClickStats.Dashboard.v1";
inline constexpr UINT kFlushMessage = WM_APP + 20;
inline constexpr UINT kStatusMessage = WM_APP + 21;
inline constexpr UINT kPauseMessage = WM_APP + 22;
inline constexpr UINT kOpenMessage = WM_APP + 23;
inline constexpr UINT kExitMessage = WM_APP + 24;
inline constexpr UINT kReadyMessage = WM_APP + 25;
inline constexpr UINT kDiagnoseMessage = WM_APP + 26;
inline constexpr UINT kClearMessage = WM_APP + 27, kClearStateMessage = WM_APP + 28;
inline constexpr UINT kRecent30Message = WM_APP + 29, kRecent60Message = WM_APP + 30, kRecentAgeMessage = WM_APP + 31;
inline constexpr UINT kCollectorExitingMessage = WM_APP + 93;
inline constexpr UINT kShowDashboardMessage = WM_APP + 94;
inline constexpr WPARAM kExitForTakeover = 1;
inline constexpr LRESULT kStatusRunning = 1, kStatusPaused = 2, kStatusStorageError = 4;
inline constexpr LRESULT kStatusElevated = 8, kStatusHigherForeground = 16;
inline constexpr LRESULT kStatusClearSupported = 32;
inline constexpr LRESULT kStatusRecentSupported = 64;
std::string Utf8(const std::wstring& value);
std::wstring Wide(const std::string& value);
std::filesystem::path ExecutablePath();
std::filesystem::path InstallationDirectory(const std::filesystem::path& executable);
std::filesystem::path DashboardExecutable(const std::filesystem::path& executable);
std::filesystem::path CollectorExecutable();
std::filesystem::path PortableDashboardPeer(const std::filesystem::path& executable);
std::filesystem::path DataDirectory();
std::filesystem::path DatabasePath();
std::string LocalDate(int daysAgo = 0);
std::string ShiftDate(const std::string& date, int offset);
HWND CollectorWindow();
LRESULT CollectorCommand(UINT message, WPARAM parameter = 0);
void Launch(const std::filesystem::path& executable, const std::wstring& arguments = L"", bool foreground = false);
DWORD ProcessIntegrity(DWORD pid = GetCurrentProcessId());
bool SameProcessUser(DWORD pid);
bool RequestElevatedCollector(HWND owner, const std::filesystem::path& collector);
bool RequestDashboardActivation(HWND dashboard, bool settings = false);
void ActivateDashboardWindow(HWND dashboard);
void LaunchDashboard(const std::filesystem::path& dashboard, bool settings = false);
std::string CsvCell(const std::string& value);

struct AppIdentity {
    std::string key = "unknown";
    std::string name = u8"未知应用";
};
inline constexpr char kTaskbarAppKey[] = "shell:taskbar";
// Name-only resolution uses a public process snapshot, without opening the target process.
AppIdentity ResolveProcessIdentity(DWORD pid, bool nameOnly = false);
class ForegroundTracker {
public:
    const AppIdentity& Current(bool force = false);
    const AppIdentity& ForWindow(HWND foreground, bool force = false);
    // Taskbar interactions belong to the shell even while foreground activation is in flight.
    const AppIdentity& ForInput(HWND foreground, HWND hitWindow, bool buttonDown = true);
private:
    HWND window_ = nullptr;
    DWORD pid_ = 0;
    ULONGLONG resolvedAt_ = 0;
    AppIdentity identity_;
};
} // namespace mouse

