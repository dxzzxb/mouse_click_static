#include "platform/windows_helpers.h"
#include <shellapi.h>
#include <ole2.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlobj.h>
#include <servprov.h>
#include <stdexcept>
#include <vector>

namespace mouse {
namespace {
struct Handle { HANDLE value{}; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
template<class T> struct Com {
    T* value{};
    ~Com() { if (value) value->Release(); }
    T* operator->() const { return value; }
    void** Address() { return reinterpret_cast<void**>(&value); }
};
void Check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("Cannot launch dashboard through Windows Explorer (HRESULT " + std::to_string(result) + ")"); }
std::vector<BYTE> TokenData(DWORD pid, TOKEN_INFORMATION_CLASS type) {
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)}, token;
    if (!process.value || !OpenProcessToken(process.value, TOKEN_QUERY, &token.value)) return {};
    DWORD bytes = 0; GetTokenInformation(token.value, type, nullptr, 0, &bytes);
    std::vector<BYTE> data(bytes);
    if (!bytes || !GetTokenInformation(token.value, type, data.data(), bytes, &bytes)) return {};
    return data;
}
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct AutomationString {
    BSTR value;
    explicit AutomationString(const std::wstring& text) : value(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) { if (!value) throw std::bad_alloc(); }
    ~AutomationString() { SysFreeString(value); }
};
}
DWORD ProcessIntegrity(DWORD pid) {
    auto data = TokenData(pid, TokenIntegrityLevel);
    if (data.empty()) return 0;
    const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data.data())->Label.Sid;
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}
bool SameProcessUser(DWORD pid) {
    auto target = TokenData(pid, TokenUser), own = TokenData(GetCurrentProcessId(), TokenUser);
    return !target.empty() && !own.empty() && EqualSid(reinterpret_cast<TOKEN_USER*>(target.data())->User.Sid,
        reinterpret_cast<TOKEN_USER*>(own.data())->User.Sid);
}
bool RequestElevatedCollector(HWND owner, const std::filesystem::path& collector) {
    SHELLEXECUTEINFOW execute{}; execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC; execute.hwnd = owner;
    execute.lpVerb = L"runas"; execute.lpFile = collector.c_str(); execute.lpParameters = L"--background --take-over";
    const auto directory = collector.parent_path().wstring(); execute.lpDirectory = directory.c_str(); execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        const auto error = GetLastError();
        if (error == ERROR_CANCELLED) return false;
        throw std::runtime_error("Cannot request collector elevation (Win32 " + std::to_string(error) + ")");
    }
    if (execute.hProcess) CloseHandle(execute.hProcess);
    return true;
}
bool RequestDashboardActivation(HWND dashboard, bool settings) {
    DWORD pid = 0;
    if (!dashboard || !GetWindowThreadProcessId(dashboard, &pid) || !SameProcessUser(pid)) return false;
    AllowSetForegroundWindow(pid);
    return PostMessageW(dashboard, kShowDashboardMessage, settings ? 1 : 0, 0) != FALSE;
}
void ActivateDashboardWindow(HWND dashboard) {
    // This runs on the window's own thread after the caller grants foreground permission.
    ShowWindow(dashboard, IsIconic(dashboard) ? SW_RESTORE : SW_SHOW);
    BringWindowToTop(dashboard);
    if (!SetForegroundWindow(dashboard)) {
        FLASHWINFO flash{sizeof(flash), dashboard, FLASHW_TRAY, 3, 0}; FlashWindowEx(&flash);
    }
}
void LaunchDashboard(const std::filesystem::path& dashboard, bool settings) {
    if (auto existing = FindWindowW(nullptr, L"Mouse Click Stats"); existing && RequestDashboardActivation(existing, settings)) return;
    if (ProcessIntegrity() < SECURITY_MANDATORY_HIGH_RID) { Launch(dashboard, settings ? L"--settings" : L"", true); return; }
    // Use Explorer's existing desktop token; never fall back to an elevated WinUI process.
    Apartment apartment;
    Com<IShellWindows> windows; Check(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, windows.Address()));
    VARIANT empty{}; long hwnd = 0;
    Com<IDispatch> desktop; Check(windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop.value));
    Com<IServiceProvider> provider; Check(desktop->QueryInterface(IID_IServiceProvider, provider.Address()));
    Com<IShellBrowser> browser; Check(provider->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, browser.Address()));
    Com<IShellView> view; Check(browser->QueryActiveShellView(&view.value));
    Com<IDispatch> background; Check(view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, background.Address()));
    Com<IShellFolderViewDual> folder; Check(background->QueryInterface(IID_IShellFolderViewDual, folder.Address()));
    Com<IDispatch> application; Check(folder->get_Application(&application.value));
    Com<IShellDispatch2> shell; Check(application->QueryInterface(IID_IShellDispatch2, shell.Address()));
    // Pass the user's activation to the ordinary-permission Explorer COM server.
    CoAllowSetForegroundWindow(shell.value, nullptr);
    DWORD explorerPid = 0; GetWindowThreadProcessId(GetShellWindow(), &explorerPid);
    if (explorerPid) AllowSetForegroundWindow(explorerPid);
    AutomationString file(dashboard.wstring());
    AutomationString parameters(settings ? L"--settings" : L"");
    VARIANT arguments{}; arguments.vt = VT_BSTR; arguments.bstrVal = parameters.value;
    VARIANT show{}; show.vt = VT_I4; show.lVal = SW_SHOWNORMAL;
    Check(shell->ShellExecute(file.value, arguments, empty, empty, show));
}
} // namespace mouse
