#include "ui/dashboard.h"
#include "platform/windows_helpers.h"
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <memory>
#include <fstream>
#include <shellapi.h>
#include <sstream>
#ifdef MOUSE_FRAMEWORK_DEPENDENT
#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>
#endif

// No custom WinRT types or XAML compiler are needed: controls are loaded from an embedded layout.
// Metadata for built-in WinUI controls is delegated to the SDK's provider.
namespace {
int exitCode = 0;
void ReportFailure(const std::filesystem::path& directory, const std::wstring& text, const wchar_t* title) {
    exitCode = 1;
    if (directory.empty()) MessageBoxW(nullptr, text.c_str(), title, MB_OK | MB_ICONERROR);
    else std::ofstream(directory / L"error.txt") << mouse::Utf8(text);
}
}
struct App : winrt::Microsoft::UI::Xaml::ApplicationT<App, winrt::Microsoft::UI::Xaml::Markup::IXamlMetadataProvider> {
    explicit App(std::filesystem::path directory, bool autoVerify, bool settings) : verificationDirectory_(std::move(directory)), autoVerify_(autoVerify), settings_(settings) {
        UnhandledException([this](auto const&, winrt::Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& error) {
            error.Handled(true);
            ReportFailure(verificationDirectory_, error.Message().c_str(), L"Mouse Click Stats · 界面错误");
            winrt::Microsoft::UI::Xaml::Application::Current().Exit();
        });
    }
    void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
        try {
            Resources().MergedDictionaries().Append(winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources());
            dashboard_ = std::make_shared<Dashboard>(verificationDirectory_, autoVerify_, settings_);
            dashboard_->Show();
        } catch (const winrt::hresult_error& error) {
            auto text = std::wstring(error.message().c_str()) + L"\nHRESULT: " + std::to_wstring(error.code().value);
            ReportFailure(verificationDirectory_, text, L"Mouse Click Stats · 布局加载失败");
            Exit();
        } catch (const std::exception& error) {
            ReportFailure(verificationDirectory_, mouse::Wide(error.what()), L"Mouse Click Stats · 布局加载失败");
            Exit();
        }
    }
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(winrt::hstring const& name) { return provider_.GetXamlType(name); }
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type) { return provider_.GetXamlType(type); }
    winrt::com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider_.GetXmlnsDefinitions(); }
private:
    winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider_;
    std::shared_ptr<Dashboard> dashboard_;
    std::filesystem::path verificationDirectory_;
    bool autoVerify_ = true;
    bool settings_ = false;
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::filesystem::path verificationDirectory;
    bool autoVerify = true;
    bool settings = false;
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    if (argc == 2 && (std::wstring(argv[1]) == L"--close" || std::wstring(argv[1]) == L"--diagnose")) {
        const auto message = std::wstring(argv[1]) == L"--close" ? WM_CLOSE : WM_APP + 91;
        LocalFree(argv);
        if (auto hwnd = FindWindowW(nullptr, L"Mouse Click Stats")) {
            DWORD_PTR result = 0;
            return SendMessageTimeoutW(hwnd, message, 0, 0, SMTO_ABORTIFHUNG, 3000, &result) ? 0 : 3;
        }
        return 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--settings") settings = true;
    else if (argc > 1) {
        if (argc != 3 || (std::wstring(argv[1]) != L"--verify" && std::wstring(argv[1]) != L"--preview")) { LocalFree(argv); return 2; }
        autoVerify = std::wstring(argv[1]) == L"--verify";
        verificationDirectory = argv[2];
        if (!verificationDirectory.is_absolute() || !std::filesystem::is_directory(verificationDirectory)) { LocalFree(argv); return 2; }
    }
    LocalFree(argv);
#ifdef MOUSE_FRAMEWORK_DEPENDENT
    // Prefer the same-version local bundle before taking the dashboard mutex.
    // Taking the mutex first would make the child UI exit as a duplicate instance.
    if (verificationDirectory.empty()) {
        try {
            if (auto peer = mouse::PortableDashboardPeer(mouse::ExecutablePath()); !peer.empty()) {
                mouse::LaunchDashboard(peer, settings);
                return 0;
            }
        } catch (const std::exception& error) {
            ReportFailure({}, mouse::Wide(error.what()), L"Mouse Click Stats · 启动便携界面失败");
            return 1;
        }
    }
#endif
    auto mutex = CreateMutexW(nullptr, FALSE, verificationDirectory.empty() ? mouse::kDashboardMutex : L"Local\\MouseClickStats.UI.Verify.v1");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (verificationDirectory.empty()) {
            if (auto hwnd = FindWindowW(nullptr, L"Mouse Click Stats")) mouse::RequestDashboardActivation(hwnd, settings);
        }
        CloseHandle(mutex); return 0;
    }
    int result = 0;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
#ifdef MOUSE_FRAMEWORK_DEPENDENT
        PACKAGE_VERSION minimum{};
        minimum.Version = WINDOWSAPPSDK_RUNTIME_VERSION_UINT64;
        auto bootstrap = MddBootstrapInitialize(WINDOWSAPPSDK_RELEASE_MAJORMINOR, WINDOWSAPPSDK_RELEASE_VERSION_TAG_W, minimum);
        if (FAILED(bootstrap)) {
            std::wostringstream detail;
            detail << L"当前启动的是共享运行时版（small），此版本不包含 WinUI 运行库。\n\n"
                   << L"请使用完整的 portable 文件夹，并运行其中的 MouseClickStats.exe。\n"
                   << L"单独使用 small 版需要安装匹配的 Windows App Runtime。\n\n"
                   << L"启动目录：" << mouse::InstallationDirectory(mouse::ExecutablePath()).wstring()
                   << L"\n错误代码：0x" << std::hex << std::uppercase << static_cast<unsigned long>(bootstrap);
            ReportFailure(verificationDirectory,
                detail.str(), L"Mouse Click Stats · 共享运行时不可用");
            CloseHandle(mutex);
            return 1;
        }
#endif
        winrt::Microsoft::UI::Xaml::Application::Start([&](auto&&) { winrt::make<App>(verificationDirectory, autoVerify, settings); });
        result = exitCode;
    } catch (const winrt::hresult_error& error) {
        ReportFailure(verificationDirectory, error.message().c_str(), L"Mouse Click Stats · 界面启动失败"); result = 1;
    } catch (const std::exception& error) {
        ReportFailure(verificationDirectory, mouse::Wide(error.what()), L"Mouse Click Stats · 界面启动失败"); result = 1;
    }
    CloseHandle(mutex);
#ifdef MOUSE_FRAMEWORK_DEPENDENT
    MddBootstrapShutdown();
#endif
    return result;
}
