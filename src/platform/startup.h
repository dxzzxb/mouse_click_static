#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace mouse {
enum class StartupMode { Disabled, Normal, Elevated };
StartupMode ReadStartupMode();
// False means the user cancelled UAC. Only this explicitly invoked operation changes startup.
bool ChangeStartupMode(HWND owner, StartupMode mode, const std::filesystem::path& collector);
// Used by the same-account elevated configuration helper, before collector initialization.
void ConfigureStartupMode(StartupMode mode, const std::filesystem::path& collector);
std::wstring CurrentUserSid();
std::wstring ElevatedStartupTaskXml(const std::filesystem::path& collector, const std::wstring& sid);
void ValidateStartupTaskForTesting(const std::filesystem::path& collector);
}
