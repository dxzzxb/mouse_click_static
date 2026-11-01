#pragma once
#include <windows.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include "core/dashboard_model.h"
#include "core/recent_activity.h"
#include "core/collector_lifecycle.h"
#include "core/click_speed_test.h"
#include "platform/startup.h"
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <functional>
#include <memory>
#include <map>
#include <chrono>
#include <deque>
#include <set>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

class Dashboard : public std::enable_shared_from_this<Dashboard> {
public:
    explicit Dashboard(std::filesystem::path verificationDirectory = {}, bool autoVerify = true, bool settings = false);
    void Show();
private:
    template<class T> T Find(const wchar_t* name) const { return root_.FindName(name).as<T>(); }
    struct Snapshot { mouse::DashboardModel model; mouse::LifetimeStats lifetime; mouse::RecentSummary recent; LRESULT status = 0; mouse::StartupMode startup = mouse::StartupMode::Disabled; std::wstring error, startupError; };
    void Refresh(bool force = false);
    winrt::fire_and_forget RefreshAsync(bool force);
    winrt::fire_and_forget CommandAsync(UINT command, bool enabled);
    winrt::fire_and_forget ChangeStartup();
    void ApplySnapshot(Snapshot result, bool force);
    void Render(bool force);
    void DrawTrend();
    void DrawButtons();
    void DrawTopApps();
    void DrawActivity();
    void DrawLifetime();
    void UpdateTitleBar();
    winrt::fire_and_forget ShowSpeedTest();
    void ResetSpeedTest();
    void UpdateSpeedTest();
    void SpeedTestPress(int button);
    void UpdateApplications(bool reorder = false);
    void FillRow(winrt::Microsoft::UI::Xaml::Controls::Grid const& row, const std::string& key);
    void ContainerChanged(winrt::Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const& args);
    void ShowInspector(const std::string& key);
    void RenderInspector();
    void RequestIcon(const std::string& key);
    void PumpIcons();
    std::wstring AppName(const mouse::AppMetrics& app) const;
    winrt::fire_and_forget LoadIconAsync(std::string key);
    void Navigate(int page);
    winrt::fire_and_forget Export();
    winrt::fire_and_forget Diagnose();
    winrt::fire_and_forget Elevate();
    winrt::fire_and_forget ClearStatistics();
    void Notice(const std::wstring& text, bool error = true);
    void Safe(const std::function<void()>& action);
    void LayoutSoon();
    void FinishMoving();
    void WriteUiDiagnostics();
    static LRESULT CALLBACK WindowSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    winrt::fire_and_forget Verify();
    winrt::Windows::Foundation::IAsyncAction SaveScreenshot(const wchar_t* name);
    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement root_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog speedDialog_{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer speedTimer_{nullptr};
    mouse::ClickSpeedTest speedTest_;
    mouse::LifetimeStats lifetime_;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer layoutTimer_{nullptr}, searchTimer_{nullptr}, heartbeat_{nullptr};
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> appItems_{nullptr};
    std::map<std::string, winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::Grid>> realized_;
    std::vector<std::string> orderedKeys_;
    std::map<std::string, winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap> icons_;
    std::map<std::string, std::wstring> appNames_;
    std::set<std::string> requestedIcons_;
    std::deque<std::string> iconQueue_;
    unsigned iconJobs_ = 0;
    mouse::DashboardModel model_;
    mouse::RecentSummary recent_, verificationRecent_{6500, 9200, 3600000};
    mouse::CollectorWatch collectorWatch_;
    bool recentSupported_ = false, dirtyPulse_ = true;
    std::unique_ptr<Snapshot> deferred_;
    HWND hwnd_{}, originalForeground_{};
    std::filesystem::path verificationDirectory_, database_;
    std::string date_, selectedApp_;
    int range_ = 7, page_ = 0, trendFilter_ = 0;
    uint64_t generation_ = 0;
    bool updating_ = false, updatingRows_ = false, followingToday_ = true, refreshing_ = false, refreshAgain_ = false;
    bool closed_ = false, moving_ = false, secondaryMonitor_ = false, connect_ = true, forcePending_ = false;
    bool dirtyOverview_ = true, dirtyApplications_ = true, draggingDeferred_ = false;
    bool autoVerify_ = true;
    bool elevating_ = false;
    bool startupChanging_ = false, startupDirty_ = false, openSettings_ = false;
    mouse::StartupMode verificationStartup_ = mouse::StartupMode::Disabled;
    LRESULT collectorStatus_ = 0, verificationStatus_ = 0;
    bool clearing_ = false;
    uint64_t nativeTicks_ = 0, startedReads_ = 0, moveEnters_ = 0, moveExits_ = 0, moveMessages_ = 0;
    ULONGLONG readStartedAt_ = 0, appliedAt_ = 0;
    double worstMoveMessageMs_ = 0;
    bool dataWarning_ = false;
    size_t overviewRenders_ = 0;
    size_t listResets_ = 0, containersCreated_ = 0, applyCount_ = 0, skippedUnchanged_ = 0;
    double worstApplyMs_ = 0, worstHeartbeatMs_ = 0;
    std::chrono::steady_clock::time_point lastHeartbeat_{};
};
