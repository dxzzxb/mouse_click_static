#include "platform/startup.h"
#include "platform/windows_helpers.h"
#include <taskschd.h>
#include <shellapi.h>
#include <sddl.h>
#include <optional>
#include <vector>
#include <stdexcept>

namespace mouse {
namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
template<class T> struct Com {
    T* value{};
    ~Com() { if (value) value->Release(); }
    T* operator->() const { return value; }
    T** Put() { return &value; }
};
struct Bstr {
    BSTR value{};
    Bstr() = default;
    explicit Bstr(const std::wstring& text) : value(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) { if (!value) throw std::bad_alloc(); }
    ~Bstr() { SysFreeString(value); }
    std::wstring Text() const { return value ? std::wstring(value, SysStringLen(value)) : L""; }
};
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
void Check(HRESULT result, const char* operation = "task operation") {
    if (FAILED(result)) throw std::runtime_error(u8"无法访问登录自启计划任务（" + std::string(operation) + ", HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ")");
}
bool Missing(HRESULT value) { return value == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || value == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND); }
std::wstring Escape(const std::wstring& text) {
    std::wstring out;
    for (const auto c : text) switch (c) {
        case L'&': out += L"&amp;"; break; case L'<': out += L"&lt;"; break; case L'>': out += L"&gt;"; break;
        case L'\"': out += L"&quot;"; break; case L'\'': out += L"&apos;"; break; default: out += c;
    }
    return out;
}
std::wstring TaskName() { return L"MouseClickStats-Admin-" + CurrentUserSid(); }
bool CurrentAccount(const std::wstring& user) {
    const auto own = CurrentUserSid();
    if (user == own) return true;
    DWORD bytes = 0, characters = 0; SID_NAME_USE kind{};
    LookupAccountNameW(nullptr, user.c_str(), nullptr, &bytes, nullptr, &characters, &kind);
    if (!bytes) return false;
    std::vector<BYTE> sid(bytes); std::vector<wchar_t> domain(characters + 1);
    if (!LookupAccountNameW(nullptr, user.c_str(), sid.data(), &bytes, domain.data(), &characters, &kind)) return false;
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid.data(), &text)) return false;
    const bool matches = own == text; LocalFree(text); return matches;
}
struct Tasks {
    Apartment apartment;
    Com<ITaskService> service;
    Com<ITaskFolder> folder;
    Tasks() {
        Check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>(service.Put())), "TaskScheduler");
        VARIANT empty{}; Check(service->Connect(empty, empty, empty, empty), "Connect");
        Bstr root(L"\\"); Check(service->GetFolder(root.value, folder.Put()), "GetFolder");
    }
    bool Find(Com<IRegisteredTask>& task) {
        Bstr name(TaskName()); auto result = folder->GetTask(name.value, task.Put());
        if (Missing(result)) return false;
        Check(result);
        // Never update or remove a foreign task that happens to have the same name.
        Com<ITaskDefinition> definition; Check(task->get_Definition(definition.Put()));
        Com<IRegistrationInfo> info; Check(definition->get_RegistrationInfo(info.Put()));
        Bstr source; Check(info->get_Source(&source.value));
        Com<IPrincipal> principal; Check(definition->get_Principal(principal.Put()));
        Bstr user; Check(principal->get_UserId(&user.value));
        if (source.Text() != L"MouseClickStats" || !CurrentAccount(user.Text()))
            throw std::runtime_error(u8"同名自启任务不属于当前账户的 Mouse Click Stats，未做修改。");
        return true;
    }
    void Write(const std::wstring& xml, LONG flags = TASK_CREATE_OR_UPDATE | TASK_IGNORE_REGISTRATION_TRIGGERS) {
        Bstr name(TaskName()), content(xml), sid(CurrentUserSid());
        VARIANT user{}; user.vt = VT_BSTR; user.bstrVal = sid.value;
        VARIANT empty{}; Com<IRegisteredTask> task;
        Check(folder->RegisterTask(name.value, content.value, flags, user, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, task.Put()), flags == TASK_VALIDATE_ONLY ? "ValidateTask" : "RegisterTask");
    }
    void Remove() {
        Bstr name(TaskName()); const auto result = folder->DeleteTask(name.value, 0);
        if (!Missing(result)) Check(result, "DeleteTask");
    }
};
std::optional<std::wstring> ReadRun() {
    DWORD bytes = 0;
    auto error = RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"MouseClickStats", RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
    if (error != ERROR_SUCCESS) throw std::runtime_error(u8"无法读取登录自启注册表项。");
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1);
    error = RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"MouseClickStats", RRF_RT_REG_SZ, nullptr, value.data(), &bytes);
    if (error != ERROR_SUCCESS) throw std::runtime_error(u8"无法读取登录自启注册表项。");
    return std::wstring(value.data());
}
void WriteRun(const std::optional<std::wstring>& command) {
    HKEY key{};
    auto error = command ? RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
                         : RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (!command && error == ERROR_FILE_NOT_FOUND) return;
    if (error != ERROR_SUCCESS) throw std::runtime_error(u8"无法写入登录自启注册表项。");
    error = command ? RegSetValueExW(key, L"MouseClickStats", 0, REG_SZ, reinterpret_cast<const BYTE*>(command->c_str()), static_cast<DWORD>((command->size() + 1) * sizeof(wchar_t)))
                    : RegDeleteValueW(key, L"MouseClickStats");
    RegCloseKey(key);
    if (error != ERROR_SUCCESS && (command || error != ERROR_FILE_NOT_FOUND)) throw std::runtime_error(u8"无法保存登录自启配置。");
}
}
std::wstring CurrentUserSid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Cannot read current user token");
    DWORD bytes = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> data(bytes);
    const bool read = bytes && GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!read || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid)) throw std::runtime_error("Cannot identify current Windows account");
    std::wstring result(sid); LocalFree(sid); return result;
}
std::wstring ElevatedStartupTaskXml(const std::filesystem::path& collector, const std::wstring& sid) {
    if (!collector.is_absolute() || sid.empty()) throw std::invalid_argument("Invalid startup executable or account");
    const auto user = Escape(sid);
    return LR"(<?xml version="1.0" encoding="UTF-16"?><Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task"><RegistrationInfo><Source>MouseClickStats</Source><Description>当前用户登录后，以管理员权限启动 Mouse Click Stats 后台采集器。</Description></RegistrationInfo><Triggers><LogonTrigger><Enabled>true</Enabled><UserId>)" + user +
        LR"(</UserId></LogonTrigger></Triggers><Principals><Principal id="Collector"><UserId>)" + user +
        LR"(</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals><Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>false</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable><RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable><Enabled>true</Enabled><Hidden>false</Hidden><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority></Settings><Actions Context="Collector"><Exec><Command>)" + Escape(collector.wstring()) +
        L"</Command><Arguments>--background --take-over</Arguments><WorkingDirectory>" + Escape(collector.parent_path().wstring()) + L"</WorkingDirectory></Exec></Actions></Task>";
}
StartupMode ReadStartupMode() {
    Tasks tasks; Com<IRegisteredTask> task;
    if (tasks.Find(task)) {
        VARIANT_BOOL enabled{}; Check(task->get_Enabled(&enabled));
        if (enabled) return StartupMode::Elevated;
    }
    return ReadRun() ? StartupMode::Normal : StartupMode::Disabled;
}
void ConfigureStartupMode(StartupMode mode, const std::filesystem::path& collector) {
    if (mode != StartupMode::Disabled && mode != StartupMode::Normal && mode != StartupMode::Elevated) throw std::invalid_argument("Invalid startup mode");
    if (!collector.is_absolute() || !std::filesystem::is_regular_file(collector)) throw std::runtime_error("Collector executable is missing");
    Tasks tasks; Com<IRegisteredTask> task; const bool hadTask = tasks.Find(task);
    const auto oldRun = ReadRun(); Bstr oldXml;
    if (hadTask) Check(task->get_Xml(&oldXml.value));
    if ((hadTask || mode == StartupMode::Elevated) && ProcessIntegrity() < SECURITY_MANDATORY_HIGH_RID)
        throw std::runtime_error(u8"修改管理员自启需要 Windows UAC 授权。");
    try {
        if (mode == StartupMode::Elevated) { tasks.Write(ElevatedStartupTaskXml(collector, CurrentUserSid())); WriteRun({}); }
        else {
            WriteRun(mode == StartupMode::Normal ? std::optional<std::wstring>(L"\"" + collector.wstring() + L"\" --background") : std::nullopt);
            if (hadTask) tasks.Remove();
        }
    } catch (...) {
        // Preserve the old entry if migrating between the Run key and scheduled task fails.
        try {
            if (hadTask) tasks.Write(oldXml.Text()); else if (mode == StartupMode::Elevated) tasks.Remove();
            WriteRun(oldRun);
        } catch (...) { throw std::runtime_error(u8"自启配置失败且未能完整恢复原配置，请重新检查登录自启设置。"); }
        throw;
    }
}
bool ChangeStartupMode(HWND owner, StartupMode mode, const std::filesystem::path& collector) {
    if (mode != StartupMode::Disabled && mode != StartupMode::Normal && mode != StartupMode::Elevated) throw std::invalid_argument("Invalid startup mode");
    Tasks tasks; Com<IRegisteredTask> task;
    const bool needsAdmin = mode == StartupMode::Elevated || tasks.Find(task);
    if (!needsAdmin || ProcessIntegrity() >= SECURITY_MANDATORY_HIGH_RID) { ConfigureStartupMode(mode, collector); return true; }
    const auto label = mode == StartupMode::Elevated ? L"elevated" : mode == StartupMode::Normal ? L"normal" : L"off";
    const auto args = L"--set-startup " + std::wstring(label) + L" --startup-user " + CurrentUserSid();
    const auto directory = collector.parent_path().wstring();
    SHELLEXECUTEINFOW execute{}; execute.cbSize = sizeof(execute); execute.hwnd = owner;
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC; execute.lpVerb = L"runas";
    execute.lpFile = collector.c_str(); execute.lpParameters = args.c_str(); execute.lpDirectory = directory.c_str(); execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        if (GetLastError() == ERROR_CANCELLED) return false;
        throw std::runtime_error(u8"无法请求 Windows 授权设置管理员自启。");
    }
    const auto waited = WaitForSingleObject(execute.hProcess, 60000); DWORD code = 1;
    const bool completed = waited == WAIT_OBJECT_0 && GetExitCodeProcess(execute.hProcess, &code);
    CloseHandle(execute.hProcess);
    if (!completed || code != 0) throw std::runtime_error(u8"登录自启设置未完成；请使用当前 Windows 账户授权，并检查任务计划程序（代码 " + std::to_string(code) + ")。");
    return true;
}
void ValidateStartupTaskForTesting(const std::filesystem::path& collector) {
    Tasks tasks;
    Com<IRegisteredTask> before; const bool existed = tasks.Find(before); Bstr original;
    if (existed) Check(before->get_Xml(&original.value));
    tasks.Write(ElevatedStartupTaskXml(collector, CurrentUserSid()), TASK_VALIDATE_ONLY);
    Com<IRegisteredTask> after; const bool exists = tasks.Find(after); Bstr current;
    if (exists) Check(after->get_Xml(&current.value));
    if (existed != exists || original.Text() != current.Text()) throw std::runtime_error("Validate-only changed startup task state");
}
}
