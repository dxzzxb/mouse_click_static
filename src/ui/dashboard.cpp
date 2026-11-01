#include "ui/dashboard.h"
#include "platform/windows_helpers.h"
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <winver.h>
#include <dwmapi.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <thread>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;
using namespace Microsoft::UI::Xaml::Media;
using namespace Windows::Foundation;
namespace Shapes = winrt::Microsoft::UI::Xaml::Shapes;
namespace {
constexpr unsigned kColors[]{0x11AA9A, 0x559EEA, 0x9C87EC, 0xE1AF4F, 0xE580AD};
constexpr UINT_PTR kDataTimer = 0x4D435354;
constexpr UINT kUiDiagnose = WM_APP + 91, kAfterMove = WM_APP + 92;
constexpr const wchar_t* kButtons[]{L"左键", L"右键", L"中键", L"侧键 1", L"侧键 2"};
struct __declspec(uuid("905a0fef-bc53-11df-8c49-001e4fc686da")) BufferBytes : ::IUnknown {
    virtual HRESULT __stdcall Buffer(uint8_t**) = 0;
};
SolidColorBrush Ink(unsigned rgb, uint8_t alpha = 255) {
    return SolidColorBrush(Windows::UI::Color{alpha, static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)});
}
std::wstring Number(int64_t value) {
    auto text = std::to_wstring(value);
    for (int pos = static_cast<int>(text.size()) - 3; pos > 0; pos -= 3) text.insert(pos, L",");
    return text;
}
std::wstring Decimal(double value, int precision = 1) {
    std::wostringstream out; out << std::fixed << std::setprecision(precision) << value; return out.str();
}
double Notches(const mouse::Counts& value) { return (static_cast<double>(value.wheelUp) + value.wheelDown) / 120.0; }
std::wstring Wheel(const mouse::Counts& value) { auto n = Notches(value); return n == std::floor(n) ? Number(static_cast<int64_t>(n)) : Decimal(n); }
std::wstring Compare(int64_t now, int64_t before) {
    if (now == before) return L"与前一天持平";
    if (!before) return now ? L"前一天暂无点击记录" : L"与前一天持平";
    const double percent = 100.0 * (static_cast<double>(now) - before) / before;
    return (percent > 0 ? L"↑ " : percent < 0 ? L"↓ " : L"— ") + Decimal(std::abs(percent)) + L"%  较前一天";
}
void SetText(TextBlock const& text, const std::wstring& value) { if (text.Text() != value) text.Text(value); }
TextBlock Label(const std::wstring& value, double size = 12) { TextBlock text; text.Text(value); text.FontSize(size); return text; }
void Put(Canvas const& canvas, UIElement const& item, double x, double y) { Canvas::SetLeft(item, x); Canvas::SetTop(item, y); canvas.Children().Append(item); }
std::string DateString(DateTime value) {
    Windows::Globalization::Calendar calendar; calendar.ChangeCalendarSystem(Windows::Globalization::CalendarIdentifiers::Gregorian()); calendar.SetDateTime(value);
    char date[11]{}; std::snprintf(date, sizeof(date), "%04d-%02d-%02d", calendar.Year(), calendar.Month(), calendar.Day()); return date;
}
std::wstring Lower(std::wstring text) { if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size())); return text; }
bool Dark(FrameworkElement const& root) { return root.ActualTheme() == ElementTheme::Dark; }
void Breakdown(StackPanel const& panel, const mouse::Counts& counts, bool dark) {
    const auto total = counts.TotalClicks();
    if (!panel.Children().Size()) for (size_t i = 0; i < 5; ++i) {
        StackPanel row; row.Spacing(7);
        Grid header; ColumnDefinition left; left.Width({1, GridUnitType::Star}); header.ColumnDefinitions().Append(left);
        ColumnDefinition right; right.Width({1, GridUnitType::Auto}); header.ColumnDefinitions().Append(right);
        StackPanel title; title.Orientation(Orientation::Horizontal); title.Spacing(7);
        Shapes::Ellipse dot; dot.Width(6); dot.Height(6); dot.Fill(Ink(kColors[i])); title.Children().Append(dot); title.Children().Append(Label(kButtons[i]));
        header.Children().Append(title); auto value = Label(L"", 11); Grid::SetColumn(value, 1); header.Children().Append(value); row.Children().Append(header);
        Border track; track.Height(4); track.CornerRadius({2,2,2,2});
        Border fill; fill.Height(4); fill.CornerRadius({2,2,2,2}); fill.HorizontalAlignment(HorizontalAlignment::Left); fill.Background(Ink(kColors[i])); track.Child(fill);
        row.Children().Append(track); panel.Children().Append(row);
    }
    for (size_t i = 0; i < 5; ++i) {
        auto row = panel.Children().GetAt(static_cast<uint32_t>(i)).as<StackPanel>();
        auto header = row.Children().GetAt(0).as<Grid>();
        const double share = total ? static_cast<double>(counts.clicks[i]) / total : 0;
        SetText(header.Children().GetAt(1).as<TextBlock>(), Number(counts.clicks[i]) + L"  ·  " + Decimal(share * 100) + L"%");
        auto track = row.Children().GetAt(1).as<Border>(); track.Background(Ink(dark ? 0x2B3948 : 0xE8EEF3));
        auto fill = track.Child().as<Border>(); fill.Width((track.ActualWidth() > 0 ? track.ActualWidth() : 240) * share);
    }
}
int64_t Value(const mouse::Counts& value, int filter) { return filter > 0 && filter <= 5 ? value.clicks[filter - 1] : value.TotalClicks(); }
void Plot(Canvas const& canvas, const std::vector<mouse::Counts>& data, const std::vector<std::string>& dates, int filter, bool dark) {
    const double width = canvas.ActualWidth(), height = canvas.Height();
    if (width < 80 || data.empty()) return;
    const double left = 48, right = width - 12, top = 14, bottom = height - 25;
    const auto layout = std::to_wstring(static_cast<int>(width)) + L":" + std::to_wstring(static_cast<int>(height)) + L":" + std::to_wstring(data.size()) + (dark ? L"d" : L"l") + mouse::Wide(dates.front());
    bool rebuild = !canvas.Tag() || unbox_value<hstring>(canvas.Tag()) != layout;
    if (rebuild) {
        canvas.Children().Clear(); canvas.Tag(box_value(layout));
        for (int i = 0; i < 3; ++i) {
            Shapes::Line line; line.X1(left); line.X2(right); line.Y1(top + (bottom - top) * i / 2); line.Y2(line.Y1()); line.Stroke(Ink(dark ? 0x2B3948 : 0xE4EBF1)); canvas.Children().Append(line);
            auto label = Label(L"", 10); label.Opacity(0.65); Put(canvas, label, 0, line.Y1() - 7);
        }
        Shapes::Polygon area; area.Fill(Ink(0x13B5A2, dark ? 35 : 22)); canvas.Children().Append(area);
        Shapes::Polyline curve; curve.Stroke(Ink(dark ? 0x43D2BF : 0x00998B)); curve.StrokeThickness(2.3); curve.StrokeLineJoin(PenLineJoin::Round); canvas.Children().Append(curve);
        Shapes::Ellipse dot; dot.Width(7); dot.Height(7); dot.Fill(curve.Stroke()); canvas.Children().Append(dot);
        auto empty = Label(L"所选时段暂无点击记录", 12); empty.Width(width); empty.TextAlignment(TextAlignment::Center); empty.Opacity(0.65); Put(canvas, empty, 0, height / 2 - 12);
        for (size_t i = 0; i < data.size(); ++i) if (data.size() <= 7 || i == 0 || i == data.size() - 1 || i % (data.size() / 5) == 0) {
            auto label = Label(mouse::Wide(dates[i].substr(5)), 10); label.Opacity(0.65);
            Put(canvas, label, left + (right - left) * i / (data.size() - 1) - 14, bottom + 9);
        }
    }
    int64_t maximum = 2, total = 0;
    for (const auto& day : data) { maximum = (std::max)(maximum, Value(day, filter)); if (Value(day, filter)) total = 1; }
    for (int i = 0; i < 3; ++i) {
        auto label = canvas.Children().GetAt(i * 2 + 1).as<TextBlock>();
        const double n = maximum * (2.0 - i) / 2;
        SetText(label, !total ? L"" : n >= 10000 ? Decimal(n / 10000) + L"万" : Number(static_cast<int64_t>(n)));
    }
    PointCollection points, filled;
    filled.Append({static_cast<float>(left), static_cast<float>(bottom)});
    for (size_t i = 0; i < data.size(); ++i) {
        Point point{static_cast<float>(left + (right - left) * i / (data.size() - 1)), static_cast<float>(bottom - (bottom - top) * static_cast<double>(Value(data[i], filter)) / maximum)};
        points.Append(point); filled.Append(point);
    }
    filled.Append({static_cast<float>(right), static_cast<float>(bottom)});
    auto area = canvas.Children().GetAt(6).as<Shapes::Polygon>(); area.Points(filled); area.Visibility(total ? Visibility::Visible : Visibility::Collapsed);
    auto curve = canvas.Children().GetAt(7).as<Shapes::Polyline>(); curve.Points(points); curve.Visibility(total ? Visibility::Visible : Visibility::Collapsed);
    auto dot = canvas.Children().GetAt(8).as<Shapes::Ellipse>(); dot.Visibility(total ? Visibility::Visible : Visibility::Collapsed);
    Canvas::SetLeft(dot, points.GetAt(static_cast<uint32_t>(data.size() - 1)).X - 3.5); Canvas::SetTop(dot, points.GetAt(static_cast<uint32_t>(data.size() - 1)).Y - 3.5);
    canvas.Children().GetAt(9).as<TextBlock>().Visibility(total ? Visibility::Collapsed : Visibility::Visible);
}
struct IconData { std::vector<uint8_t> pixels; std::wstring name; };
IconData ExtractIcon(const std::string& app) {
    auto path = mouse::Wide(app); IconData output;
    const bool taskbar = app == mouse::kTaskbarAppKey;
    DWORD unused = 0;
    if (auto bytes = GetFileVersionInfoSizeW(path.c_str(), &unused); bytes > 0 && bytes < 4 * 1024 * 1024) {
        std::vector<uint8_t> version(bytes);
        if (GetFileVersionInfoW(path.c_str(), 0, bytes, version.data())) {
            struct Translation { WORD language, page; }; Translation* translations = nullptr; UINT length = 0;
            if (VerQueryValueW(version.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) && length >= sizeof(Translation)) {
                wchar_t query[100]{}; swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].language, translations[0].page);
                wchar_t* value = nullptr; UINT characters = 0;
                if (VerQueryValueW(version.data(), query, reinterpret_cast<void**>(&value), &characters) && value && characters > 1) output.name = value;
            }
        }
    }
    HICON icon = nullptr, smallIcon = nullptr;
    if (taskbar) {
        SHSTOCKICONINFO stock{}; stock.cbSize = sizeof(stock);
        if (SUCCEEDED(SHGetStockIconInfo(SIID_DESKTOPPC, SHGSI_ICON | SHGSI_LARGEICON, &stock))) icon = stock.hIcon;
        output.name = L"Windows 任务栏";
    } else ExtractIconExW(path.c_str(), 0, &icon, &smallIcon, 1);
    if (!icon) { icon = smallIcon; smallIcon = nullptr; }
    if (smallIcon) DestroyIcon(smallIcon);
    if (!icon) icon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    if (icon) {
        constexpr int size = 48;
        BITMAPINFO bitmap{}; bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bitmap.bmiHeader.biWidth = size; bitmap.bmiHeader.biHeight = -size;
        bitmap.bmiHeader.biPlanes = 1; bitmap.bmiHeader.biBitCount = 32; bitmap.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr; auto dc = CreateCompatibleDC(nullptr); auto image = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (image && dc && pixels) {
            auto previous = SelectObject(dc, image); std::memset(pixels, 0, size * size * 4); DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
            auto* bytes = static_cast<uint8_t*>(pixels); output.pixels.assign(bytes, bytes + size * size * 4);
            bool alpha = false; for (size_t i = 3; i < output.pixels.size(); i += 4) if (output.pixels[i]) { alpha = true; break; }
            if (!alpha) for (size_t i = 0; i < output.pixels.size(); i += 4) if (output.pixels[i] || output.pixels[i+1] || output.pixels[i+2]) output.pixels[i+3] = 255;
            SelectObject(dc, previous);
        }
        if (image) DeleteObject(image); if (dc) DeleteDC(dc); DestroyIcon(icon);
    }
    return output;
}
IAsyncAction WaitLayout(int milliseconds = 600) { co_await resume_after(std::chrono::milliseconds(milliseconds)); }
uint64_t MonotonicUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
IInspectable LoadLayout(int id) {
    auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) throw std::runtime_error("Layout resource is missing");
    const auto* bytes = static_cast<const char*>(LockResource(LoadResource(nullptr, resource)));
    return Markup::XamlReader::Load(mouse::Wide(std::string(bytes, SizeofResource(nullptr, resource))));
}
}

Dashboard::Dashboard(std::filesystem::path directory, bool autoVerify, bool settings) : verificationDirectory_(std::move(directory)), autoVerify_(autoVerify), openSettings_(settings) {
    verificationStatus_ = mouse::kStatusRunning | mouse::kStatusRecentSupported;
    database_ = verificationDirectory_.empty() ? mouse::DatabasePath() : verificationDirectory_ / L"stats.db";
    date_ = mouse::LocalDate(); window_ = Window(); window_.Title(verificationDirectory_.empty() ? L"Mouse Click Stats" : L"Mouse Click Stats · 预览");
    root_ = LoadLayout(101).as<FrameworkElement>();
    window_.Content(root_);
    check_hresult(window_.as<IWindowNative>()->get_WindowHandle(&hwnd_));
    if (!verificationDirectory_.empty()) SetPropW(hwnd_, L"MouseClickStats.Verification", reinterpret_cast<HANDLE>(1));
    window_.ExtendsContentIntoTitleBar(true);
    window_.SetTitleBar(Find<Grid>(L"AppTitleBar"));
    window_.AppWindow().TitleBar().PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Tall);
    window_.Activated([this](auto const&, WindowActivatedEventArgs const& event) {
        Find<StackPanel>(L"TitleBrand").Opacity(event.WindowActivationState() == WindowActivationState::Deactivated ? 0.55 : 1);
    });
    if (!ChangeWindowMessageFilterEx(hwnd_, mouse::kCollectorExitingMessage, MSGFLT_ALLOW, nullptr))
        throw std::runtime_error("Cannot configure dashboard shutdown notification");
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor); GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
    if (!verificationDirectory_.empty()) {
        struct Selection { MONITORINFO info{}; bool found = false; } selection;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR handle, HDC, LPRECT, LPARAM value) -> BOOL {
            auto& selected = *reinterpret_cast<Selection*>(value); MONITORINFO info{}; info.cbSize = sizeof(info);
            if (GetMonitorInfoW(handle, &info) && !(info.dwFlags & MONITORINFOF_PRIMARY)) { selected.info = info; selected.found = true; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&selection));
        if (selection.found) { monitor = selection.info; secondaryMonitor_ = true; }
    }
    const double scale = GetDpiForWindow(hwnd_) / 96.0;
    const auto width = (std::min)(static_cast<int>(1380 * scale), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    const auto height = (std::min)(static_cast<int>(920 * scale), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    SetWindowPos(hwnd_, nullptr, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
                 monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    auto icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1)); SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon)); SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    SetWindowSubclass(hwnd_, WindowSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
    auto picker = Find<CalendarDatePicker>(L"DatePicker"); picker.Date(clock::now());
    picker.DateChanged([this](auto const&, CalendarDatePickerDateChangedEventArgs const& event) {
        if (!event.NewDate()) return;
        Safe([&] { date_ = DateString(event.NewDate().Value()); followingToday_ = date_ == mouse::LocalDate(); ++generation_; Refresh(true); });
    });
    Find<Button>(L"TodayButton").Click([this](auto const&, auto const&) { Find<CalendarDatePicker>(L"DatePicker").Date(clock::now()); followingToday_ = true; });
    Find<ComboBox>(L"RangePicker").SelectionChanged([this](auto const&, auto const&) { range_ = Find<ComboBox>(L"RangePicker").SelectedIndex() == 0 ? 7 : Find<ComboBox>(L"RangePicker").SelectedIndex() == 1 ? 30 : 90; ++generation_; Refresh(true); });
    Find<ComboBox>(L"TrendFilter").SelectionChanged([this](auto const&, auto const&) { trendFilter_ = Find<ComboBox>(L"TrendFilter").SelectedIndex(); Safe([&] { DrawTrend(); }); });
    Find<ListView>(L"Navigation").SelectionChanged([this](auto const&, auto const&) { Safe([&] { Navigate(Find<ListView>(L"Navigation").SelectedIndex()); }); });
    Find<Button>(L"AllAppsButton").Click([this](auto const&, auto const&) { Find<ListView>(L"Navigation").SelectedIndex(1); });
    layoutTimer_ = root_.DispatcherQueue().CreateTimer(); layoutTimer_.Interval(std::chrono::milliseconds(140)); layoutTimer_.IsRepeating(false);
    layoutTimer_.Tick([this](auto const&, auto const&) { if (!closed_ && !moving_) Safe([&] { if (page_ == 0) { DrawTrend(); DrawButtons(); } if (!selectedApp_.empty()) RenderInspector(); }); });
    searchTimer_ = root_.DispatcherQueue().CreateTimer(); searchTimer_.Interval(std::chrono::milliseconds(160)); searchTimer_.IsRepeating(false);
    searchTimer_.Tick([this](auto const&, auto const&) { Safe([&] { UpdateApplications(true); }); });
    Find<TextBox>(L"AppSearch").TextChanged([this](auto const&, auto const&) { searchTimer_.Stop(); searchTimer_.Start(); });
    Find<ComboBox>(L"SortPicker").SelectionChanged([this](auto const&, auto const&) { Safe([&] { UpdateApplications(true); }); });
    auto list = Find<ListView>(L"AppList"); appItems_ = single_threaded_observable_vector<IInspectable>(); list.ItemsSource(appItems_);
    list.ContainerContentChanging([this](auto const&, ContainerContentChangingEventArgs const& args) { Safe([&] { ContainerChanged(args); }); });
    list.ItemClick([this](auto const&, ItemClickEventArgs const& args) { Safe([&] { ShowInspector(to_string(unbox_value<hstring>(args.ClickedItem()))); }); });
    list.SelectionChanged([this](auto const&, auto const&) {
        if (updatingRows_) return; auto item = Find<ListView>(L"AppList").SelectedItem(); if (item) Safe([&] { ShowInspector(to_string(unbox_value<hstring>(item))); });
    });
    Find<Button>(L"CloseInspector").Click([this](auto const&, auto const&) { selectedApp_.clear(); Find<Border>(L"Inspector").Visibility(Visibility::Collapsed); });
    auto pause = Find<ToggleButton>(L"PauseButton");
    const auto pauseChanged = [this] { if (!updating_) { auto state = Find<ToggleButton>(L"PauseButton").IsChecked(); CommandAsync(mouse::kPauseMessage, state && state.Value()); } };
    pause.Checked([pauseChanged](auto const&, auto const&) { pauseChanged(); }); pause.Unchecked([pauseChanged](auto const&, auto const&) { pauseChanged(); });
    Find<ComboBox>(L"StartupModePicker").SelectionChanged([this](auto const&, auto const&) {
        if (!updating_) { startupDirty_ = true; SetText(Find<TextBlock>(L"StartupHint"), L"选择尚未应用。点击“应用设置”保存登录自启权限。"); }
    });
    Find<Button>(L"SaveStartupButton").Click([this](auto const&, auto const&) { ChangeStartup(); });
    Find<Button>(L"FolderButton").Click([this](auto const&, auto const&) { Safe([&] { ShellExecuteW(hwnd_, L"open", database_.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }); });
    Find<Button>(L"ExportButton").Click([this](auto const&, auto const&) { Export(); });
    Find<Button>(L"DiagnoseButton").Click([this](auto const&, auto const&) { Diagnose(); });
    Find<Button>(L"ElevateButton").Click([this](auto const&, auto const&) { Elevate(); });
    Find<Button>(L"BannerElevateButton").Click([this](auto const&, auto const&) { Elevate(); });
    Find<Button>(L"ClearStatisticsButton").Click([this](auto const&, auto const&) { ClearStatistics(); });
    Find<Button>(L"SpeedTestButton").Click([this](auto const&, auto const&) { ShowSpeedTest(); });
    Find<Canvas>(L"TrendChart").SizeChanged([this](auto const&, auto const&) { LayoutSoon(); });
    root_.ActualThemeChanged([this](auto const&, auto const&) { UpdateTitleBar(); dirtyOverview_ = dirtyPulse_ = true; DrawLifetime(); LayoutSoon(); });
    BOOL dark = Dark(root_); DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    UpdateTitleBar();
    window_.Closed([this](auto const&, auto const&) { closed_ = true; KillTimer(hwnd_, kDataTimer); layoutTimer_.Stop(); searchTimer_.Stop(); if (heartbeat_) heartbeat_.Stop(); if (speedTimer_) speedTimer_.Stop(); if (speedDialog_) speedDialog_.Hide(); RemoveWindowSubclass(hwnd_, WindowSubclass, 1); Application::Current().Exit(); });
    if (!verificationDirectory_.empty()) {
        Find<ComboBox>(L"StartupModePicker").IsEnabled(false); Find<Button>(L"SaveStartupButton").IsEnabled(false); Find<ToggleButton>(L"PauseButton").IsEnabled(false); Find<Button>(L"ExportButton").IsEnabled(false); Find<Button>(L"FolderButton").IsEnabled(false);
        Find<Button>(L"DiagnoseButton").IsEnabled(false);
        Find<Button>(L"ElevateButton").IsEnabled(false);
        Find<Button>(L"ClearStatisticsButton").IsEnabled(false);
        Notice(L"示例数据预览 · 不影响真实统计", false);
    }
}
void Dashboard::Show() {
    originalForeground_ = GetForegroundWindow();
    if (!verificationDirectory_.empty()) ShowWindow(hwnd_, autoVerify_ ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL); else window_.Activate();
    if (openSettings_) Find<ListView>(L"Navigation").SelectedIndex(2);
    Refresh(true); if (!SetTimer(hwnd_, kDataTimer, 1000, nullptr)) throw std::runtime_error("Cannot start dashboard refresh timer");
    if (!verificationDirectory_.empty() && autoVerify_) Verify();
}
void Dashboard::Safe(const std::function<void()>& action) {
    try { action(); } catch (const hresult_error& error) { Notice(L"操作未完成：" + std::wstring(error.message().c_str())); } catch (const std::exception& error) { Notice(mouse::Wide(error.what())); }
}
void Dashboard::Notice(const std::wstring& text, bool error) { auto bar = Find<InfoBar>(L"Notice"); if (bar.Message() != text) bar.Message(text); bar.Severity(error ? InfoBarSeverity::Warning : InfoBarSeverity::Informational); bar.IsOpen(true); }
void Dashboard::Refresh(bool force) {
    if (closed_) return;
    forcePending_ |= force;
    if (clearing_) return;
    if (moving_) { draggingDeferred_ = true; return; }
    if (refreshing_) { refreshAgain_ = true; return; }
    if (followingToday_ && date_ != mouse::LocalDate()) { Find<CalendarDatePicker>(L"DatePicker").Date(clock::now()); return; }
    RefreshAsync(forcePending_); forcePending_ = false;
}
fire_and_forget Dashboard::RefreshAsync(bool force) {
    auto lifetime = shared_from_this(); apartment_context ui; refreshing_ = true; ++startedReads_; readStartedAt_ = GetTickCount64();
    const auto date = date_; const auto range = range_; const auto request = generation_; const auto database = database_; const auto directory = verificationDirectory_; const auto verificationStatus = verificationStatus_; const auto verificationRecent = verificationRecent_; const auto verificationStartup = verificationStartup_; const bool connect = connect_; connect_ = false;
    Snapshot result;
    co_await resume_background();
    try {
        if (directory.empty()) {
            if (connect && !mouse::CollectorWindow()) mouse::Launch(mouse::CollectorExecutable(), L"--background");
            result.status = mouse::CollectorCommand(mouse::kStatusMessage); mouse::CollectorCommand(mouse::kFlushMessage);
            try { result.startup = mouse::ReadStartupMode(); } catch (const std::exception& error) { result.startupError = mouse::Wide(error.what()); }
            if (result.status & mouse::kStatusRecentSupported) {
                result.recent.halfHour = mouse::CollectorCommand(mouse::kRecent30Message);
                result.recent.hour = mouse::CollectorCommand(mouse::kRecent60Message);
                result.recent.observedMs = static_cast<uint64_t>(mouse::CollectorCommand(mouse::kRecentAgeMessage));
            }
        } else { result.status = verificationStatus; result.recent = verificationRecent; result.startup = verificationStartup; if (std::filesystem::exists(directory / L"slow-read")) std::this_thread::sleep_for(std::chrono::milliseconds(350)); }
        mouse::SqliteStore store(database, true);
        result.model = mouse::BuildDashboardModel(store.Read(mouse::ShiftDate(date, (std::min)(-1, 1 - range)), date), date, range);
        result.lifetime = store.ReadLifetime();
    } catch (const std::exception& error) { result.error = mouse::Wide(error.what()); }
    co_await ui;
    if (closed_) co_return;
    refreshing_ = false;
    if (request != generation_) { Refresh(true); co_return; }
    if (moving_) { deferred_ = std::make_unique<Snapshot>(std::move(result)); forcePending_ |= force; draggingDeferred_ = true; }
    else Safe([&] { ApplySnapshot(std::move(result), force); });
    if (refreshAgain_) { refreshAgain_ = false; Refresh(forcePending_); }
}
void Dashboard::ApplySnapshot(Snapshot result, bool force) {
    const auto begin = std::chrono::steady_clock::now(); ++applyCount_; appliedAt_ = GetTickCount64();
    collectorStatus_ = result.status;
    recent_ = result.recent; recentSupported_ = (result.status & mouse::kStatusRecentSupported) != 0; dirtyPulse_ = true;
    updating_ = true;
    const bool paused = (result.status & mouse::kStatusPaused) != 0;
    auto pauseButton = Find<ToggleButton>(L"PauseButton"); pauseButton.IsChecked(box_value(paused).as<IReference<bool>>());
    const hstring pauseCaption = paused ? L"继续统计" : L"暂停统计";
    if (!pauseButton.Content() || unbox_value<hstring>(pauseButton.Content()) != pauseCaption) pauseButton.Content(box_value(pauseCaption));
    if (!startupDirty_ && !startupChanging_) {
        Find<ComboBox>(L"StartupModePicker").SelectedIndex(static_cast<int>(result.startup));
        SetText(Find<TextBlock>(L"StartupHint"), !result.startupError.empty() ? L"无法确认已保存的自启配置：" + result.startupError : result.startup == mouse::StartupMode::Elevated ? L"已保存：管理员权限登录自启。下次登录自动提权，仅启动后台采集器。" : result.startup == mouse::StartupMode::Normal ? L"已保存：普通权限登录自启。手动提升权限仅对本次采集生效。" : L"已关闭登录自启。选择权限后点击“应用设置”保存。");
    }
    const bool startupAvailable = verificationDirectory_.empty() && !startupChanging_ && result.startupError.empty();
    Find<ComboBox>(L"StartupModePicker").IsEnabled(startupAvailable); Find<Button>(L"SaveStartupButton").IsEnabled(startupAvailable);
    updating_ = false;
    const bool elevated = (result.status & mouse::kStatusElevated) != 0;
    const bool connected = (result.status & mouse::kStatusRunning) != 0;
    const bool permissionGap = (result.status & mouse::kStatusHigherForeground) && !elevated;
    SetText(Find<TextBlock>(L"StatusLabel"), !verificationDirectory_.empty() ? L"示例数据" : !result.status ? L"采集器未连接" : paused ? L"已暂停" : permissionGap ? L"需提升采集权限" : elevated ? L"正在统计 · 管理员" : L"正在统计");
    SetText(Find<TextBlock>(L"PermissionHint"), !connected ? L"采集器未连接，暂时无法确认权限。连接恢复后可提升采集权限。" : elevated ? L"后台采集器：管理员权限。统计界面仍以普通权限运行。" : permissionGap ? L"检测到权限更高的应用，部分点击可能漏记。请按需提升采集器权限。" : L"后台采集器：普通权限。以管理员运行的游戏需要采集器具有相同权限。");
    Find<Button>(L"ElevateButton").Visibility(elevated ? Visibility::Collapsed : Visibility::Visible);
    const bool canElevate = connected && !elevated && verificationDirectory_.empty() && !elevating_;
    Find<Button>(L"ElevateButton").IsEnabled(canElevate);
    Find<Button>(L"BannerElevateButton").IsEnabled(canElevate);
    auto banner = Find<InfoBar>(L"PermissionBanner");
    banner.IsOpen(connected && !elevated);
    banner.Title(permissionGap ? L"检测到权限更高的应用" : L"采集器尚未以管理员权限运行");
    banner.Message(permissionGap ? L"部分点击可能漏记，建议提升采集权限。" : L"普通应用可正常统计；管理员运行的游戏或程序可能漏记点击。");
    const bool canClear = (result.status & mouse::kStatusClearSupported) != 0 || !verificationDirectory_.empty();
    Find<Button>(L"ClearStatisticsButton").IsEnabled(verificationDirectory_.empty() && canClear && !clearing_);
    SetText(Find<TextBlock>(L"ClearStatisticsHint"), canClear ? L"删除全部日期、全部应用的统计记录，确认后执行。" : L"请先从托盘退出旧版后台，再启动新版本以使用此功能。");
    Find<Shapes::Ellipse>(L"StatusDot").Fill(Ink(!connected ? 0x91A2B4 : paused || permissionGap ? 0xE1AF4F : 0x11AA9A));
    if (!result.error.empty()) { dataWarning_ = true; Notice(L"暂时无法读取统计，稍后自动重试。"); return; }
    if (result.status & mouse::kStatusStorageError) { dataWarning_ = true; Notice(L"统计保存出现异常，请检查磁盘空间与数据目录。"); }
    else if (dataWarning_) { dataWarning_ = false; Find<InfoBar>(L"Notice").IsOpen(false); }
    if (force || lifetime_.firstDate != result.lifetime.firstDate || !mouse::SameCounts(lifetime_.counts, result.lifetime.counts)) {
        lifetime_ = std::move(result.lifetime); DrawLifetime();
    }
    if (force || model_.signature != result.model.signature || model_.date.empty()) {
        model_ = std::move(result.model);
        if (!selectedApp_.empty() && !model_.FindApp(selectedApp_)) { selectedApp_.clear(); Find<Border>(L"Inspector").Visibility(Visibility::Collapsed); }
        dirtyOverview_ = dirtyApplications_ = true; Render(force);
    } else ++skippedUnchanged_;
    DrawActivity();
    SYSTEMTIME now{}; GetLocalTime(&now); wchar_t time[80]{}; swprintf_s(time, L"%02u:%02u:%02u 已同步", now.wHour, now.wMinute, now.wSecond);
    SetText(Find<TextBlock>(L"RefreshLabel"), time);
    worstApplyMs_ = (std::max)(worstApplyMs_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
}
fire_and_forget Dashboard::CommandAsync(UINT command, bool enabled) {
    auto lifetime = shared_from_this(); apartment_context ui; std::wstring error;
    co_await resume_background();
    try { if (!mouse::CollectorCommand(command, enabled)) throw std::runtime_error("Collector is not responding"); } catch (const std::exception& e) { error = mouse::Wide(e.what()); }
    co_await ui; if (closed_) co_return; if (!error.empty()) Notice(error); Refresh();
}
fire_and_forget Dashboard::ChangeStartup() {
    if (startupChanging_ || !verificationDirectory_.empty()) co_return;
    const auto index = Find<ComboBox>(L"StartupModePicker").SelectedIndex();
    if (index < 0 || index > 2) co_return;
    auto lifetime = shared_from_this(); apartment_context ui;
    const auto mode = static_cast<mouse::StartupMode>(index); const auto collector = mouse::CollectorExecutable();
    startupChanging_ = true; ++generation_; Find<ComboBox>(L"StartupModePicker").IsEnabled(false); Find<Button>(L"SaveStartupButton").IsEnabled(false);
    bool saved = false; std::wstring error;
    co_await resume_background();
    try { saved = mouse::ChangeStartupMode(hwnd_, mode, collector); } catch (const std::exception& e) { error = mouse::Wide(e.what()); }
    co_await ui; if (closed_) co_return;
    startupChanging_ = startupDirty_ = false; ++generation_;
    Notice(!error.empty() ? error : !saved ? L"已取消 Windows 授权，登录自启设置未更改。" : mode == mouse::StartupMode::Elevated ? L"已保存管理员权限登录自启，下次登录生效。当前采集权限可用下方按钮提升。" : mode == mouse::StartupMode::Normal ? L"已保存普通权限登录自启，下次登录生效。" : L"已关闭登录自启。当前采集器继续运行。", !error.empty());
    Refresh(true);
}
void Dashboard::Render(bool force) {
    if (moving_ || model_.dates.empty()) return;
    DrawActivity();
    if (page_ == 0 && dirtyOverview_) {
        ++overviewRenders_;
        SetText(Find<TextBlock>(L"TotalClicks"), Number(model_.selected.TotalClicks())); SetText(Find<TextBlock>(L"DayComparison"), Compare(model_.selected.TotalClicks(), model_.previous.TotalClicks()));
        SetText(Find<TextBlock>(L"WheelCount"), Wheel(model_.selected)); SetText(Find<TextBlock>(L"WheelDetail"), L"↑ " + Decimal(model_.selected.wheelUp / 120.0) + L"   ↓ " + Decimal(model_.selected.wheelDown / 120.0));
        SetText(Find<TextBlock>(L"AverageLabel"), L"近 " + std::to_wstring(range_) + L" 天日均"); SetText(Find<TextBlock>(L"AverageClicks"), Number(static_cast<int64_t>(std::round(model_.DailyAverage()))));
        SetText(Find<TextBlock>(L"PeakLabel"), model_.period.Empty() ? L"尚无活动记录" : L"峰值 " + mouse::Wide(model_.dates[model_.peakDay].substr(5)) + L" · " + Number(model_.days[model_.peakDay].TotalClicks()));
        DrawTrend(); DrawButtons(); DrawTopApps(); dirtyOverview_ = false;
    }
    if (page_ == 1 && dirtyApplications_) { UpdateApplications(force); dirtyApplications_ = false; }
    if (!selectedApp_.empty()) RenderInspector();
}
void Dashboard::Navigate(int page) {
    page_ = page;
    Find<ScrollViewer>(L"MainScroll").Visibility(page == 0 ? Visibility::Visible : Visibility::Collapsed);
    Find<Grid>(L"ApplicationsPage").Visibility(page == 1 ? Visibility::Visible : Visibility::Collapsed);
    Find<ScrollViewer>(L"SettingsPage").Visibility(page == 2 ? Visibility::Visible : Visibility::Collapsed);
    auto dateToolbar = Find<Grid>(L"DateToolbar");
    dateToolbar.Parent().as<Border>().Child(nullptr);
    Find<Border>(page == 1 ? L"ApplicationsDateHost" : L"OverviewDateHost").Child(dateToolbar);
    Find<Grid>(L"DateToolbar").Visibility(page == 2 ? Visibility::Collapsed : Visibility::Visible);
    Find<Button>(L"ExportButton").Visibility(page == 2 ? Visibility::Collapsed : Visibility::Visible);
    if (page != 1) { selectedApp_.clear(); Find<Border>(L"Inspector").Visibility(Visibility::Collapsed); }
    Render(true); LayoutSoon();
}
void Dashboard::UpdateTitleBar() {
    const bool dark = Dark(root_);
    BOOL immersive = dark; DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &immersive, sizeof(immersive));
    root_.as<Grid>().Background(Ink(dark ? 0x11171F : 0xF2F5F8));
    auto bar = window_.AppWindow().TitleBar();
    const auto color = [](unsigned rgb, uint8_t alpha = 255) { return box_value(Ink(rgb, alpha).Color()).as<IReference<Windows::UI::Color>>(); };
    bar.ButtonBackgroundColor(color(0, 0)); bar.ButtonInactiveBackgroundColor(color(0, 0));
    bar.ButtonForegroundColor(color(dark ? 0xE8EEF3 : 0x202832));
    bar.ButtonInactiveForegroundColor(color(dark ? 0x687789 : 0x91A2B4));
    bar.ButtonHoverBackgroundColor(color(dark ? 0x2B3948 : 0xDFE7ED));
    bar.ButtonHoverForegroundColor(color(dark ? 0xFFFFFF : 0x202832));
    bar.ButtonPressedBackgroundColor(color(dark ? 0x39495A : 0xCFD9E2));
    bar.ButtonPressedForegroundColor(color(dark ? 0xFFFFFF : 0x202832));
}
void Dashboard::DrawLifetime() {
    SetText(Find<TextBlock>(L"LifetimeTotal"), Number(lifetime_.counts.TotalClicks()));
    SetText(Find<TextBlock>(L"LifetimeSince"), lifetime_.firstDate.empty() ? L"暂无历史记录 · 不受日期筛选影响" : L"自 " + mouse::Wide(lifetime_.firstDate) + L" 起记录 · 不受日期筛选影响");
    auto grid = Find<Grid>(L"LifetimeButtons");
    if (!grid.Children().Size()) for (int i = 0; i < 5; ++i) {
        StackPanel item; item.Spacing(5); Grid::SetColumn(item, i);
        auto title = Label(kButtons[i], 12); item.Children().Append(title);
        auto value = Label(L"0", 24); value.FontFamily(FontFamily(L"Bahnschrift")); value.FontWeight(Windows::UI::Text::FontWeights::SemiBold()); item.Children().Append(value);
        grid.Children().Append(item);
    }
    for (int i = 0; i < 5; ++i) {
        auto item = grid.Children().GetAt(i).as<StackPanel>();
        item.Children().GetAt(0).as<TextBlock>().Foreground(Ink(Dark(root_) ? 0x91A2B4 : 0x637383));
        SetText(item.Children().GetAt(1).as<TextBlock>(), Number(lifetime_.counts.clicks[i]));
    }
}
fire_and_forget Dashboard::ShowSpeedTest() {
    if (speedDialog_) co_return;
    auto lifetime = shared_from_this();
    try {
        speedDialog_ = LoadLayout(102).as<ContentDialog>();
        speedDialog_.XamlRoot(root_.XamlRoot()); speedDialog_.RequestedTheme(root_.ActualTheme());
        speedTimer_ = root_.DispatcherQueue().CreateTimer(); speedTimer_.Interval(std::chrono::milliseconds(16));
        speedTimer_.Tick([this](auto const&, auto const&) { Safe([&] { UpdateSpeedTest(); }); });
        auto area = speedDialog_.FindName(L"TestArea").as<Border>();
        area.PointerPressed([this](auto const&, Input::PointerRoutedEventArgs const& event) {
            if (event.Pointer().PointerDeviceType() != Microsoft::UI::Input::PointerDeviceType::Mouse) return;
            auto point = event.GetCurrentPoint(speedDialog_.FindName(L"TestArea").as<Border>());
            const auto kind = point.Properties().PointerUpdateKind();
            if (kind == Microsoft::UI::Input::PointerUpdateKind::LeftButtonPressed) SpeedTestPress(0);
            else if (kind == Microsoft::UI::Input::PointerUpdateKind::RightButtonPressed) SpeedTestPress(1);
            event.Handled(true);
        });
        const auto reset = [this](auto const&, auto const&) { Safe([&] { ResetSpeedTest(); }); };
        speedDialog_.FindName(L"TestButtonPicker").as<ComboBox>().SelectionChanged(reset);
        speedDialog_.FindName(L"TestDurationPicker").as<ComboBox>().SelectionChanged(reset);
        speedDialog_.FindName(L"TestResetButton").as<Button>().Click(reset);
        ResetSpeedTest();
        co_await speedDialog_.ShowAsync();
    } catch (const hresult_error& error) { if (!closed_) Notice(L"无法打开手速测试：" + std::wstring(error.message().c_str())); }
      catch (const std::exception& error) { if (!closed_) Notice(mouse::Wide(error.what())); }
    if (speedTimer_) speedTimer_.Stop();
    speedTimer_ = nullptr; speedDialog_ = nullptr;
}
void Dashboard::ResetSpeedTest() {
    speedTimer_.Stop();
    const int button = speedDialog_.FindName(L"TestButtonPicker").as<ComboBox>().SelectedIndex();
    const int seconds = speedDialog_.FindName(L"TestDurationPicker").as<ComboBox>().SelectedIndex() == 0 ? 1 : 3;
    speedTest_.Reset(seconds, button);
    UpdateSpeedTest();
}
void Dashboard::SpeedTestPress(int button) {
    if (speedTest_.Press(button, MonotonicUs())) speedTimer_.Start();
    UpdateSpeedTest();
}
void Dashboard::UpdateSpeedTest() {
    if (!speedDialog_) return;
    const auto now = MonotonicUs(); speedTest_.Advance(now);
    const bool running = speedTest_.Status() == mouse::ClickSpeedTest::State::Running;
    const bool done = speedTest_.Status() == mouse::ClickSpeedTest::State::Finished;
    auto text = [this](const wchar_t* name, const std::wstring& value) { SetText(speedDialog_.FindName(name).as<TextBlock>(), value); };
    text(L"TestClicks", Number(speedTest_.Clicks()));
    text(L"TestCps", done ? Decimal(speedTest_.Cps(), 2) : L"—");
    text(L"TestApm", done ? Decimal(speedTest_.Apm(), 0) : L"—");
    text(L"TestAreaTitle", done ? L"测试完成" : running ? L"继续点击" : L"点击此处开始");
    const auto seconds = speedTest_.RemainingUs(now) / 1000000.0;
    text(L"TestRemaining", done ? L"点击“重新测试”开始下一轮" : running ? L"剩余 " + Decimal(seconds, 2) + L" 秒" : L"准备就绪 · " + std::wstring(kButtons[speedTest_.Button()]) + L" · " + Decimal(seconds, 0) + L" 秒");
    const int duration = speedDialog_.FindName(L"TestDurationPicker").as<ComboBox>().SelectedIndex() == 0 ? 1 : 3;
    speedDialog_.FindName(L"TestProgress").as<ProgressBar>().Value(100 * (1 - seconds / duration));
    speedDialog_.FindName(L"TestButtonPicker").as<ComboBox>().IsEnabled(!running);
    speedDialog_.FindName(L"TestDurationPicker").as<ComboBox>().IsEnabled(!running);
    if (!running) speedTimer_.Stop();
}
void Dashboard::DrawTrend() {
    if (model_.dates.empty() || page_ != 0 || moving_) return;
    Plot(Find<Canvas>(L"TrendChart"), model_.days, model_.dates, trendFilter_, Dark(root_));
    int64_t total = 0; for (const auto& day : model_.days) { mouse::Counts one; one.clicks[0] = Value(day, trendFilter_); mouse::Counts current; current.clicks[0] = total; current.Add(one); total = current.clicks[0]; }
    SetText(Find<TextBlock>(L"TrendDescription"), trendFilter_ ? std::wstring(kButtons[trendFilter_ - 1]) + L" · 每日点击量" : L"每日点击量");
    SetText(Find<TextBlock>(L"PeriodTotal"), L"区间累计  " + Number(total));
    SetText(Find<TextBlock>(L"PeriodDates"), mouse::Wide(model_.from.substr(5)) + L" — " + mouse::Wide(model_.date.substr(5)));
}
void Dashboard::DrawButtons() { if (page_ == 0) Breakdown(Find<StackPanel>(L"ButtonLegend"), model_.selected, Dark(root_)); }
void Dashboard::DrawActivity() {
    if (page_ != 0 || moving_ || !dirtyPulse_) return;
    dirtyPulse_ = false;
    auto card = Find<Border>(L"PulseCard");
    card.Visibility(model_.date == mouse::LocalDate() ? Visibility::Visible : Visibility::Collapsed);
    const bool connected = (collectorStatus_ & mouse::kStatusRunning) != 0;
    const bool paused = (collectorStatus_ & mouse::kStatusPaused) != 0;
    const auto mood = mouse::ActivityMood(recent_);
    const auto headline = !connected ? L"采集器暂时离线，电台先歇会儿。" : !recentSupported_ ? L"旧采集器还没学会播报手速。" : paused ? L"暂停营业，鼠鼠去茶水间了。" : mouse::ActivityHeadline(mood);
    SetText(Find<TextBlock>(L"PulseHeadline"), headline);
    SetText(Find<TextBlock>(L"Pulse30"), recentSupported_ ? Number(recent_.halfHour) : L"—");
    SetText(Find<TextBlock>(L"Pulse60"), recentSupported_ ? Number(recent_.hour) : L"—");
    SetText(Find<TextBlock>(L"PulsePace"), !connected ? L"重新连接后，接着记你的鼠鼠战绩。" : !recentSupported_ ? L"从托盘退出旧版，再运行新版即可开麦。" : paused ? L"暂停期间不计数，时间窗口照常滚动。" : recent_.observedMs < 1800000 ? L"APM 还在热身，先攒几下再开麦。" : L"鼠标 APM " + Decimal(recent_.halfHour / 30.0) + L" · 近 30 分钟均速");
    const auto minutes = (std::min)(uint64_t{60}, (recent_.observedMs + 59999) / 60000);
    SetText(Find<TextBlock>(L"PulseCoverage"), !recentSupported_ ? L"近 30 分钟 / 近 1 小时 · 等待采集器" : minutes < 60 ? L"本轮已观察 " + std::to_wstring(minutes) + L" 分钟 · 未满窗口展示已采集部分" : L"按分钟滚动 · 仅五键按下，不含滚轮与键盘");
    SetText(Find<TextBlock>(L"PulseBadge"), !connected || !recentSupported_ ? L"等信号" : paused ? L"中场休息" : mood == mouse::PulseMood::Warming ? L"热身中" : mood == mouse::PulseMood::Starcraft ? L"星际争霸职业选手！" : mood == mouse::PulseMood::Marathon ? L"鼠王降临" : mood == mouse::PulseMood::Turbo ? L"手速超频" : mood == mouse::PulseMood::Busy ? L"开始上强度" : L"松弛频道");
    auto bars = Find<StackPanel>(L"PulseBars");
    if (!bars.Children().Size()) for (int i = 0; i < 12; ++i) {
        Border bar; bar.Width(4); bar.Height(7 + (i % 4) * 4); bar.CornerRadius(CornerRadius{2,2,2,2}); bar.VerticalAlignment(VerticalAlignment::Center);
        bars.Children().Append(bar);
    }
    const int lit = !connected || !recentSupported_ || paused ? 0 : (std::min)(12, 2 + static_cast<int>(mood) * 2);
    for (uint32_t i = 0; i < bars.Children().Size(); ++i) {
        auto bar = bars.Children().GetAt(i).as<Border>(); bar.Background(Ink(Dark(root_) ? 0x43D2BF : 0x008C82)); bar.Opacity(static_cast<int>(i) < lit ? 1 : 0.17);
    }
}
std::wstring Dashboard::AppName(const mouse::AppMetrics& app) const { auto found = appNames_.find(app.key); return found != appNames_.end() && !found->second.empty() ? found->second : mouse::Wide(app.name); }
void Dashboard::DrawTopApps() {
    auto panel = Find<StackPanel>(L"TopApps");
    if (!panel.Children().Size()) for (size_t i = 0; i < 5; ++i) {
        Grid row; row.ColumnSpacing(12);
        for (GridLength length : {GridLength{30,GridUnitType::Pixel}, GridLength{1,GridUnitType::Star}, GridLength{100,GridUnitType::Pixel}, GridLength{70,GridUnitType::Pixel}}) { ColumnDefinition column; column.Width(length); row.ColumnDefinitions().Append(column); }
        Image icon; icon.Width(24); icon.Height(24); row.Children().Append(icon);
        auto name = Label(L"", 13); name.VerticalAlignment(VerticalAlignment::Center); name.TextTrimming(TextTrimming::CharacterEllipsis); Grid::SetColumn(name, 1); row.Children().Append(name);
        auto count = Label(L"", 13); count.HorizontalAlignment(HorizontalAlignment::Right); count.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(count, 2); row.Children().Append(count);
        auto share = Label(L"", 11); share.Opacity(0.6); share.HorizontalAlignment(HorizontalAlignment::Right); share.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(share, 3); row.Children().Append(share);
        panel.Children().Append(row);
    }
    for (uint32_t i = 0; i < 5; ++i) {
        auto row = panel.Children().GetAt(i).as<Grid>(); row.Visibility(i < model_.apps.size() ? Visibility::Visible : Visibility::Collapsed); if (i >= model_.apps.size()) continue;
        const auto& app = model_.apps[i]; SetText(row.Children().GetAt(1).as<TextBlock>(), AppName(app)); SetText(row.Children().GetAt(2).as<TextBlock>(), Number(app.selected.TotalClicks()));
        SetText(row.Children().GetAt(3).as<TextBlock>(), Decimal(model_.selected.TotalClicks() ? 100.0 * app.selected.TotalClicks() / model_.selected.TotalClicks() : 0) + L"%");
        if (auto found = icons_.find(app.key); found != icons_.end()) row.Children().GetAt(0).as<Image>().Source(found->second); else { row.Children().GetAt(0).as<Image>().Source(nullptr); RequestIcon(app.key); }
    }
}
void Dashboard::UpdateApplications(bool) {
    if (page_ != 1 || moving_) return;
    auto search = Lower(std::wstring(Find<TextBox>(L"AppSearch").Text())); const int sort = Find<ComboBox>(L"SortPicker").SelectedIndex();
    std::vector<const mouse::AppMetrics*> apps;
    for (const auto& app : model_.apps) if (search.empty() || Lower(AppName(app) + mouse::Wide(app.key)).find(search) != std::wstring::npos) apps.push_back(&app);
    std::sort(apps.begin(), apps.end(), [this,sort](auto a, auto b) {
        if (sort == 1) { auto left = Lower(AppName(*a)), right = Lower(AppName(*b)); return left == right ? a->key < b->key : left < right; }
        const auto left = sort == 2 ? Notches(a->selected) : static_cast<double>(a->selected.TotalClicks()); const auto right = sort == 2 ? Notches(b->selected) : static_cast<double>(b->selected.TotalClicks());
        return left == right ? a->key < b->key : left > right;
    });
    std::vector<std::string> keys; for (const auto* app : apps) keys.push_back(app->key);
    if (keys != orderedKeys_) {
        std::vector<IInspectable> items; items.reserve(keys.size()); for (const auto& key : keys) items.push_back(box_value(mouse::Wide(key)));
        updatingRows_ = true; appItems_.ReplaceAll(items); orderedKeys_ = std::move(keys); updatingRows_ = false; ++listResets_;
        auto it = std::find(orderedKeys_.begin(), orderedKeys_.end(), selectedApp_); if (it != orderedKeys_.end()) { updatingRows_ = true; Find<ListView>(L"AppList").SelectedIndex(static_cast<int>(it - orderedKeys_.begin())); updatingRows_ = false; }
    }
    for (auto it = realized_.begin(); it != realized_.end();) { if (auto row = it->second.get()) { FillRow(row, it->first); ++it; } else it = realized_.erase(it); }
    Find<TextBlock>(L"AppEmpty").Visibility(apps.empty() ? Visibility::Visible : Visibility::Collapsed);
    SetText(Find<TextBlock>(L"AppListSummary"), std::to_wstring(apps.size()) + L" 个应用  ·  当日 " + Number(model_.selected.TotalClicks()) + L" 次点击");
}
void Dashboard::ContainerChanged(ContainerContentChangingEventArgs const& args) {
    auto row = args.ItemContainer().ContentTemplateRoot().try_as<Grid>();
    if (args.InRecycleQueue()) { if (row && row.Tag()) { auto key = to_string(unbox_value<hstring>(row.Tag())); realized_.erase(key); } return; }
    if (!row) { if (args.Phase() == 0) args.RegisterUpdateCallback([this](auto const&, ContainerContentChangingEventArgs const& next) { Safe([&] { ContainerChanged(next); }); }); return; }
    const auto key = to_string(unbox_value<hstring>(args.Item()));
    if (!row.Tag()) ++containersCreated_;
    if (row.Tag()) { auto previous = to_string(unbox_value<hstring>(row.Tag())); if (previous != key) realized_.erase(previous); }
    row.Tag(box_value(mouse::Wide(key))); ToolTipService::SetToolTip(row, box_value(mouse::Wide(key)));
    realized_[key] = weak_ref<Grid>(row); FillRow(row, key); args.Handled(true);
}
void Dashboard::FillRow(Grid const& row, const std::string& key) {
    const auto* app = model_.FindApp(key); if (!app) return;
    const auto text = [&row](const wchar_t* name, const std::wstring& value) { SetText(row.FindName(name).as<TextBlock>(), value); };
    text(L"AppName", AppName(*app)); text(L"RowTotal", Number(app->selected.TotalClicks()));
    constexpr const wchar_t* fields[]{L"RowLeft",L"RowRight",L"RowMiddle",L"RowSide1",L"RowSide2"}; for (size_t i = 0; i < 5; ++i) text(fields[i], Number(app->selected.clicks[i]));
    text(L"RowWheel", Wheel(app->selected)); const double share = model_.selected.TotalClicks() ? static_cast<double>(app->selected.TotalClicks()) / model_.selected.TotalClicks() : 0;
    text(L"RowShare", Decimal(share * 100) + L"%"); auto fill = row.FindName(L"ShareFill").as<Border>(); if (fill.Width() != 64 * share) fill.Width(64 * share);
    auto image = row.FindName(L"AppIcon").as<Image>(); auto avatar = row.FindName(L"Avatar").as<FontIcon>();
    if (auto found = icons_.find(key); found != icons_.end()) { if (image.Source() != found->second) image.Source(found->second); avatar.Visibility(Visibility::Collapsed); }
    else { image.Source(nullptr); avatar.Visibility(Visibility::Visible); RequestIcon(key); }
}
void Dashboard::ShowInspector(const std::string& key) { selectedApp_ = key; Find<Border>(L"Inspector").Visibility(Visibility::Visible); RenderInspector(); LayoutSoon(); }
void Dashboard::RenderInspector() {
    const auto* app = model_.FindApp(selectedApp_); if (!app || moving_) return;
    const bool taskbar = app->key == mouse::kTaskbarAppKey;
    SetText(Find<TextBlock>(L"DetailKind"), taskbar ? L"系统交互" : app->key == "unknown" ? L"未识别来源" : L"应用");
    SetText(Find<TextBlock>(L"DetailName"), AppName(*app));
    SetText(Find<TextBlock>(L"DetailPath"), taskbar ? L"任务栏上的点击与滚轮操作，包括点击程序图标切换、还原和最小化窗口。此类操作统一归入任务栏。" : app->key == "unknown" ? L"输入发生时未能识别前台应用。历史记录无法可靠还原归属。" : mouse::Wide(app->key));
    SetText(Find<TextBlock>(L"DetailTotal"), Number(app->selected.TotalClicks()));
    SetText(Find<TextBlock>(L"DetailComparison"), Compare(app->selected.TotalClicks(), app->previous.TotalClicks()));
    SetText(Find<TextBlock>(L"DetailShare"), L"占当日全部点击的 " + Decimal(model_.selected.TotalClicks() ? 100.0 * app->selected.TotalClicks() / model_.selected.TotalClicks() : 0) + L"%");
    Plot(Find<Canvas>(L"DetailChart"), app->days, model_.dates, 0, Dark(root_)); Breakdown(Find<StackPanel>(L"DetailButtons"), app->selected, Dark(root_));
    SetText(Find<TextBlock>(L"DetailPeriod"), L"近 " + std::to_wstring(range_) + L" 天共 " + Number(app->period.TotalClicks()) + L" 次点击");
    SetText(Find<TextBlock>(L"DetailWheel"), L"滚轮向上 " + Decimal(app->selected.wheelUp / 120.0) + L" 格   /   向下 " + Decimal(app->selected.wheelDown / 120.0) + L" 格\n横向左 " + Decimal(app->selected.wheelLeft / 120.0) + L" 格   /   右 " + Decimal(app->selected.wheelRight / 120.0) + L" 格");
    if (auto found = icons_.find(app->key); found != icons_.end()) Find<Image>(L"DetailIcon").Source(found->second); else { Find<Image>(L"DetailIcon").Source(nullptr); RequestIcon(app->key); }
}
void Dashboard::RequestIcon(const std::string& key) { if (!icons_.count(key) && requestedIcons_.insert(key).second) { iconQueue_.push_back(key); PumpIcons(); } }
void Dashboard::PumpIcons() { while (!closed_ && iconJobs_ < 2 && !iconQueue_.empty()) { auto key = iconQueue_.front(); iconQueue_.pop_front(); ++iconJobs_; LoadIconAsync(std::move(key)); } }
fire_and_forget Dashboard::LoadIconAsync(std::string key) {
    auto lifetime = shared_from_this(); apartment_context ui; IconData result;
    co_await resume_background(); try { result = ExtractIcon(key); } catch (...) {}
    co_await ui; --iconJobs_; if (closed_) co_return;
    Safe([&] { if (!result.pixels.empty()) { Imaging::WriteableBitmap bitmap(48,48); uint8_t* destination = nullptr; check_hresult(bitmap.PixelBuffer().as<BufferBytes>()->Buffer(&destination)); std::memcpy(destination, result.pixels.data(), result.pixels.size()); bitmap.Invalidate(); icons_.insert_or_assign(key, bitmap); appNames_[key] = result.name;
        if (moving_) { dirtyOverview_ = dirtyApplications_ = true; return; }
        if (page_ == 0) DrawTopApps();
        if (auto row = realized_.find(key); row != realized_.end()) if (auto visual = row->second.get()) FillRow(visual, key);
        if (selectedApp_ == key) { Find<Image>(L"DetailIcon").Source(bitmap); if (auto app = model_.FindApp(key)) SetText(Find<TextBlock>(L"DetailName"), AppName(*app)); }
    }}); PumpIcons();
}
void Dashboard::LayoutSoon() { if (!closed_ && !moving_ && layoutTimer_) { layoutTimer_.Stop(); layoutTimer_.Start(); } }
fire_and_forget Dashboard::Diagnose() {
    auto lifetime = shared_from_this(); apartment_context ui;
    co_await resume_background();
    const auto result = mouse::CollectorCommand(mouse::kDiagnoseMessage);
    co_await ui;
    if (!closed_) Notice(result ? L"采集诊断已保存到数据目录的 diagnostics.json，可用“打开数据目录”查看。" : L"保存诊断失败，请确认后台正在运行且为 0.2.2 或更新版本。", !result);
}
fire_and_forget Dashboard::Elevate() {
    if (elevating_ || !verificationDirectory_.empty() || !(collectorStatus_ & mouse::kStatusRunning) || (collectorStatus_ & mouse::kStatusElevated)) co_return;
    auto lifetime = shared_from_this(); apartment_context ui;
    elevating_ = true; Find<Button>(L"ElevateButton").IsEnabled(false);
    Find<Button>(L"BannerElevateButton").IsEnabled(false);
    const auto collector = mouse::CollectorExecutable();
    bool requested = false; std::wstring error;
    co_await resume_background();
    try { requested = mouse::RequestElevatedCollector(hwnd_, collector); }
    catch (const std::exception& e) { error = mouse::Wide(e.what()); }
    co_await ui;
    if (closed_) co_return;
    elevating_ = false;
    Notice(!error.empty() ? error : requested ? L"正在以管理员权限重启后台采集器，统计数据会保留。" : L"已取消权限提升，原后台继续运行。", !error.empty());
    Refresh();
}
fire_and_forget Dashboard::ClearStatistics() {
    if (clearing_ || !verificationDirectory_.empty()) co_return;
    auto lifetime = shared_from_this(); apartment_context ui;
    std::wstring error;
    try {
        ContentDialog confirm;
        confirm.XamlRoot(root_.XamlRoot());
        confirm.Title(box_value(L"清空全部统计数据？"));
        confirm.Content(box_value(L"将删除所有日期、所有应用的鼠标统计记录。此操作不可撤销。"));
        confirm.PrimaryButtonText(L"清空统计"); confirm.CloseButtonText(L"取消");
        confirm.DefaultButton(ContentDialogButton::Close);
        if (co_await confirm.ShowAsync() != ContentDialogResult::Primary) co_return;
        clearing_ = true; ++generation_; deferred_.reset();
        Find<Button>(L"ClearStatisticsButton").IsEnabled(false); Find<Button>(L"ExportButton").IsEnabled(false);
        Notice(L"正在清空统计数据…", false);
        co_await resume_background(); const auto ticket = mouse::CollectorCommand(mouse::kClearMessage); co_await ui;
        if (!ticket) throw std::runtime_error(u8"无法请求清空，请先退出旧版后台并启动新版本。");
        bool done = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            co_await WaitLayout(150);
            co_await resume_background(); const auto state = static_cast<mouse::ClearState>(mouse::CollectorCommand(mouse::kClearStateMessage, static_cast<WPARAM>(ticket))); co_await ui;
            if (closed_) co_return;
            if (state == mouse::ClearState::Succeeded) { done = true; break; }
            if (state == mouse::ClearState::Failed) throw std::runtime_error(u8"清空失败，原有统计已保留。请检查数据目录和磁盘状态。");
            if (state == mouse::ClearState::Unknown) throw std::runtime_error(u8"无法读取清空结果，请检查后台是否仍在运行。");
        }
        if (!done) throw std::runtime_error(u8"清空操作尚未完成，请稍后检查统计结果。");
    } catch (const hresult_error& e) { error = e.message().c_str(); }
      catch (const std::exception& e) { error = mouse::Wide(e.what()); }
    if (closed_) co_return;
    clearing_ = false; ++generation_; deferred_.reset();
    Find<Button>(L"ExportButton").IsEnabled(true);
    Notice(error.empty() ? L"统计数据已清空。" : error, !error.empty()); Refresh(true);
}
void Dashboard::FinishMoving() {
    if (deferred_) { auto snapshot = std::move(deferred_); ApplySnapshot(std::move(*snapshot), forcePending_); forcePending_ = false; }
    Render(false); LayoutSoon(); Refresh();
}
void Dashboard::WriteUiDiagnostics() {
    auto directory = verificationDirectory_.empty() ? database_.parent_path() : verificationDirectory_;
    std::ofstream report(directory / L"ui-diagnostics.json");
    GUITHREADINFO thread{}; thread.cbSize = sizeof(thread); GetGUIThreadInfo(GetCurrentThreadId(), &thread);
    report << "{\"version\":\"0.3.5\",\"pid\":" << GetCurrentProcessId() << ",\"ticks\":" << nativeTicks_
        << ",\"startedReads\":" << startedReads_ << ",\"applies\":" << applyCount_ << ",\"refreshing\":" << (refreshing_ ? "true" : "false")
        << ",\"moving\":" << (moving_ ? "true" : "false") << ",\"nativeGuiFlags\":" << thread.flags
        << ",\"visible\":" << (IsWindowVisible(hwnd_) ? "true" : "false") << ",\"minimized\":" << (IsIconic(hwnd_) ? "true" : "false")
        << ",\"readStartedAt\":" << readStartedAt_ << ",\"appliedAt\":" << appliedAt_ << ",\"now\":" << GetTickCount64()
        << ",\"totalClicks\":" << model_.selected.TotalClicks() << ",\"page\":" << page_ << ",\"moveEnters\":" << moveEnters_ << ",\"moveExits\":" << moveExits_
        << ",\"moveMessages\":" << moveMessages_ << ",\"worstMoveMessageMs\":" << worstMoveMessageMs_
 << '}';
}
LRESULT CALLBACK Dashboard::WindowSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<Dashboard*>(data);
    const bool measure = message == WM_MOVING || message == WM_MOVE || message == WM_WINDOWPOSCHANGING || message == WM_WINDOWPOSCHANGED;
    const auto begin = std::chrono::steady_clock::now();
    try {
        if (message == WM_ENTERSIZEMOVE) { ++self->moveEnters_; self->moving_ = true; self->layoutTimer_.Stop(); }
        else if (message == WM_EXITSIZEMOVE) { ++self->moveExits_; self->moving_ = false; PostMessageW(window, kAfterMove, 0, 0); }
        else if (message == kAfterMove) { if (!self->moving_ && !self->closed_) self->Safe([&] { self->FinishMoving(); }); return 0; }
        else if (message == kUiDiagnose) { self->WriteUiDiagnostics(); return 1; }
        else if (message == mouse::kShowDashboardMessage) {
            if (wParam > 1 || lParam) return 0;
            self->Safe([&] { if (wParam == 1) self->Find<ListView>(L"Navigation").SelectedIndex(2); mouse::ActivateDashboardWindow(window); self->window_.Activate(); self->Refresh(true); });
            return 1;
        }
        else if (message == mouse::kCollectorExitingMessage) {
            if (!self->verificationDirectory_.empty()) std::ofstream(self->verificationDirectory_ / L"exit-notification.ok") << "received";
            PostMessageW(window, WM_CLOSE, 0, 0); return 1;
        }
        else if (message == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE) { self->Refresh(true); }
        else if (message == WM_TIMER && wParam == kDataTimer) {
            ++self->nativeTicks_;
            if (!self->closed_ && self->verificationDirectory_.empty() && self->collectorWatch_.ShouldClose(mouse::CollectorWindow() != nullptr, self->elevating_, GetTickCount64())) {
                PostMessageW(window, WM_CLOSE, 0, 0); return 0;
            }
            if (self->moving_) {
                GUITHREADINFO thread{}; thread.cbSize = sizeof(thread);
                if (GetGUIThreadInfo(GetCurrentThreadId(), &thread) && !(thread.flags & GUI_INMOVESIZE)) {
                    self->moving_ = false; PostMessageW(window, kAfterMove, 0, 0);
                }
            }
            if (!self->closed_ && IsWindowVisible(window) && !IsIconic(window) && !self->moving_) self->Refresh();
            return 0;
        }
        else if (message == WM_GETMINMAXINFO) { auto* limits = reinterpret_cast<MINMAXINFO*>(lParam); auto dpi = GetDpiForWindow(window); limits->ptMinTrackSize.x = static_cast<LONG>(1100 * dpi / 96); limits->ptMinTrackSize.y = static_cast<LONG>(640 * dpi / 96); return 0; }
    } catch (...) {}
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (measure) { ++self->moveMessages_; self->worstMoveMessageMs_ = (std::max)(self->worstMoveMessageMs_, std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - begin).count()); }
    return result;
}
fire_and_forget Dashboard::Export() {
    auto lifetime = shared_from_this(); apartment_context ui; std::filesystem::path output; const auto date = date_; const auto database = database_;
    try {
        com_ptr<IFileSaveDialog> dialog; check_hresult(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        COMDLG_FILTERSPEC filter{L"CSV 文件", L"*.csv"}; dialog->SetFileTypes(1, &filter); dialog->SetDefaultExtension(L"csv"); auto name = L"mouse-clicks-" + mouse::Wide(date) + L".csv"; dialog->SetFileName(name.c_str());
        auto result = dialog->Show(hwnd_); if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) co_return; check_hresult(result);
        com_ptr<IShellItem> item; check_hresult(dialog->GetResult(item.put())); PWSTR path = nullptr; check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path)); output = path; CoTaskMemFree(path);
    } catch (const hresult_error& error) { Notice(error.message().c_str()); co_return; }
    std::wstring error; co_await resume_background(); try { mouse::SqliteStore store(database, true); store.ExportCsv(output, date, date); } catch (const std::exception& e) { error = mouse::Wide(e.what()); }
    co_await ui; if (!closed_) Notice(error.empty() ? L"已导出所选日期的应用明细" : error, !error.empty());
}
IAsyncAction Dashboard::SaveScreenshot(const wchar_t* name) {
    Imaging::RenderTargetBitmap bitmap; co_await bitmap.RenderAsync(speedDialog_ ? speedDialog_.as<UIElement>() : root_.as<UIElement>()); auto pixels = co_await bitmap.GetPixelsAsync();
    auto reader = Windows::Storage::Streams::DataReader::FromBuffer(pixels); std::vector<uint8_t> bytes(pixels.Length()); reader.ReadBytes(bytes);
    auto folder = co_await Windows::Storage::StorageFolder::GetFolderFromPathAsync(verificationDirectory_.wstring()); auto file = co_await folder.CreateFileAsync(name, Windows::Storage::CreationCollisionOption::ReplaceExisting);
    auto stream = co_await file.OpenAsync(Windows::Storage::FileAccessMode::ReadWrite); auto encoder = co_await Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(), stream);
    encoder.SetPixelData(Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8, Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied, bitmap.PixelWidth(), bitmap.PixelHeight(), 96, 96, bytes); co_await encoder.FlushAsync();
}
fire_and_forget Dashboard::Verify() {
    auto lifetime = shared_from_this(); apartment_context ui;
    try {
        for (int i = 0; i < 80 && model_.date.empty(); ++i) co_await WaitLayout(100);
        if (model_.date.empty()) throw std::runtime_error("Statistics did not load");
        const auto lifetimeBefore = lifetime_.counts.TotalClicks();
        auto overview = Find<StackPanel>(L"OverviewPage");
        if (overview.Children().Size() != 3 || overview.Children().GetAt(0) != Find<Border>(L"PulseCard") || overview.Children().GetAt(1) != Find<StackPanel>(L"DailyStatistics") || overview.Children().GetAt(2) != Find<Border>(L"LifetimeCard"))
            throw std::runtime_error("Overview does not group realtime, date statistics, and lifetime in order");
        if (!window_.ExtendsContentIntoTitleBar() || Find<Grid>(L"LifetimeButtons").Children().Size() != 5 || Find<TextBlock>(L"LifetimeTotal").Text() != Number(lifetimeBefore) || lifetime_.firstDate != "2001-01-01")
            throw std::runtime_error("Immersive title bar or lifetime statistics did not initialize");
        root_.RequestedTheme(ElementTheme::Dark); co_await WaitLayout(1200); co_await SaveScreenshot(L"01-overview-dark.png");
        if (Find<TextBlock>(L"PulseHeadline").Text() != mouse::ActivityHeadline(mouse::PulseMood::Starcraft) || Find<TextBlock>(L"Pulse30").Text() != Number(6500) || Find<TextBlock>(L"Pulse60").Text() != Number(9200))
            throw std::runtime_error("Recent activity panel did not render the real preview snapshot");
        auto permissionBanner = Find<InfoBar>(L"PermissionBanner");
        if (!permissionBanner.IsOpen() || permissionBanner.IsClosable() || Find<Button>(L"BannerElevateButton").IsEnabled())
            throw std::runtime_error("Normal-permission preview banner is missing or allows elevation");
        Find<ListView>(L"Navigation").SelectedIndex(1); co_await WaitLayout(1200); co_await SaveScreenshot(L"02-applications.png");
        if (!model_.apps.empty()) { Find<ListView>(L"AppList").SelectedIndex(0); co_await WaitLayout(900); co_await SaveScreenshot(L"03-app-detail.png"); }
        const auto total = model_.selected.TotalClicks(); const auto apps = model_.apps.size(); const auto icons = icons_.size(); const auto containers = containersCreated_;
        selectedApp_.clear(); Find<Border>(L"Inspector").Visibility(Visibility::Collapsed);
        const auto resets = listResets_;
        const auto overviewBefore = overviewRenders_;
        heartbeat_ = root_.DispatcherQueue().CreateTimer(); heartbeat_.Interval(std::chrono::milliseconds(16)); lastHeartbeat_ = std::chrono::steady_clock::now();
        heartbeat_.Tick([this](auto const&, auto const&) { auto now = std::chrono::steady_clock::now(); worstHeartbeatMs_ = (std::max)(worstHeartbeatMs_, std::chrono::duration<double,std::milli>(now - lastHeartbeat_).count()); lastHeartbeat_ = now; }); heartbeat_.Start();
        const auto liveKey = model_.apps.front().key, liveName = model_.apps.front().name, liveDate = model_.date;
        for (int i = 0; i < 4; ++i) {
            co_await resume_background();
            { mouse::Batch change; change[{liveDate,liveKey}].name = liveName; change[{liveDate,liveKey}].counts.clicks[0] = 1; mouse::SqliteStore store(database_); store.Save(change); }
            co_await ui; co_await WaitLayout(500); // The real native timer must discover changes, including while inactive.
        }
        for (int i = 0; i < 20 && model_.selected.TotalClicks() != total + 4; ++i) co_await WaitLayout(100);
        const auto liveDelta = model_.selected.TotalClicks() - total;
        bool rowUpdated = false;
        if (auto row = realized_.find(liveKey); row != realized_.end()) if (auto visual = row->second.get()) rowUpdated = visual.FindName(L"RowTotal").as<TextBlock>().Text() == Number(model_.FindApp(liveKey)->selected.TotalClicks());
        const auto hiddenRenders = overviewRenders_ - overviewBefore;
        SendMessageW(hwnd_, WM_ENTERSIZEMOVE, 0, 0); Refresh(true); co_await WaitLayout(450); SendMessageW(hwnd_, WM_EXITSIZEMOVE, 0, 0);
        co_await WaitLayout(600); heartbeat_.Stop();
        const auto stableResets = listResets_ - resets;
        Find<ListView>(L"Navigation").SelectedIndex(0); Find<ComboBox>(L"RangePicker").SelectedIndex(2); root_.RequestedTheme(ElementTheme::Light);
        co_await WaitLayout(1000); co_await SaveScreenshot(L"04-overview-light-90days.png");
        Windows::Globalization::Calendar calendar; calendar.ChangeCalendarSystem(Windows::Globalization::CalendarIdentifiers::Gregorian()); calendar.Day(1); calendar.Month(1); calendar.Year(2020); Find<CalendarDatePicker>(L"DatePicker").Date(calendar.GetDateTime());
        co_await WaitLayout(1000); const auto empty = model_.selected.TotalClicks(); co_await SaveScreenshot(L"05-empty-day.png");
        if (Find<Border>(L"PulseCard").Visibility() != Visibility::Collapsed) throw std::runtime_error("Live activity panel leaked into a historical date");
        if (lifetime_.counts.TotalClicks() != lifetimeBefore + 4 || Find<TextBlock>(L"LifetimeTotal").Text() != Number(lifetimeBefore + 4)) throw std::runtime_error("Historical selection changed lifetime statistics or live update was lost");
        Find<CalendarDatePicker>(L"DatePicker").Date(clock::now()); Find<ListView>(L"Navigation").SelectedIndex(2); co_await WaitLayout(1000); co_await SaveScreenshot(L"06-settings.png");
        verificationStartup_ = mouse::StartupMode::Elevated; Refresh(true);
        for (int i = 0; i < 30 && Find<ComboBox>(L"StartupModePicker").SelectedIndex() != 2; ++i) co_await WaitLayout(100);
        if (Find<ComboBox>(L"StartupModePicker").SelectedIndex() != 2 || Find<Button>(L"SaveStartupButton").IsEnabled()) throw std::runtime_error("Elevated startup preview was not rendered safely");
        co_await SaveScreenshot(L"19-startup-admin.png");
        verificationStartup_ = mouse::StartupMode::Normal; Refresh(true);
        for (int i = 0; i < 30 && Find<ComboBox>(L"StartupModePicker").SelectedIndex() != 1; ++i) co_await WaitLayout(100);
        if (Find<ComboBox>(L"StartupModePicker").SelectedIndex() != 1) throw std::runtime_error("Normal startup mode did not refresh");
        Find<ComboBox>(L"StartupModePicker").SelectedIndex(2); Refresh(true); co_await WaitLayout(1000);
        if (!startupDirty_ || Find<ComboBox>(L"StartupModePicker").SelectedIndex() != 2) throw std::runtime_error("Refresh discarded an unapplied startup selection");
        startupDirty_ = false; Refresh(true); co_await WaitLayout(500);
        const auto status = [this](LRESULT value) -> IAsyncAction {
            if (value) value |= mouse::kStatusRecentSupported;
            verificationStatus_ = value; Refresh();
            for (int i = 0; i < 40 && collectorStatus_ != value; ++i) co_await WaitLayout(100);
            if (collectorStatus_ != value) throw std::runtime_error("Collector preview status did not update");
            co_await WaitLayout(100);
        };
        co_await status(mouse::kStatusRunning | mouse::kStatusPaused);
        if (!permissionBanner.IsOpen()) throw std::runtime_error("Paused collector lost the permission reminder");
        co_await status(0);
        if (permissionBanner.IsOpen() || Find<Button>(L"ElevateButton").IsEnabled()) throw std::runtime_error("Disconnected collector displayed an actionable permission reminder");
        Find<ListView>(L"Navigation").SelectedIndex(0); root_.RequestedTheme(ElementTheme::Dark);
        co_await status(mouse::kStatusRunning | mouse::kStatusHigherForeground);
        if (!permissionBanner.IsOpen() || permissionBanner.Title() != L"检测到权限更高的应用") throw std::runtime_error("Permission-gap reminder did not update");
        co_await SaveScreenshot(L"07-permission-warning.png");
        co_await status(mouse::kStatusRunning | mouse::kStatusElevated | mouse::kStatusHigherForeground);
        if (permissionBanner.IsOpen() || Find<Button>(L"ElevateButton").Visibility() != Visibility::Collapsed) throw std::runtime_error("Elevated collector retained the permission warning");
        co_await SaveScreenshot(L"08-elevated-overview.png");
        co_await status(mouse::kStatusRunning);
        Find<ListView>(L"Navigation").SelectedIndex(1); root_.RequestedTheme(ElementTheme::Light);
        if (!model_.FindApp(mouse::kTaskbarAppKey)) throw std::runtime_error("Taskbar category is missing");
        Find<ListView>(L"AppList").SelectedIndex(static_cast<int>(std::find(orderedKeys_.begin(), orderedKeys_.end(), mouse::kTaskbarAppKey) - orderedKeys_.begin()));
        ShowInspector(mouse::kTaskbarAppKey); co_await WaitLayout(600);
        if (Find<TextBlock>(L"DetailKind").Text() != L"系统交互" || Find<TextBlock>(L"DetailName").Text() != L"Windows 任务栏" || !icons_.count(mouse::kTaskbarAppKey))
            throw std::runtime_error("Taskbar category details or icon are missing");
        co_await SaveScreenshot(L"09-taskbar-detail.png");
        Find<ListView>(L"Navigation").SelectedIndex(0);
        const auto activity = [this](mouse::RecentSummary value) -> IAsyncAction {
            verificationRecent_ = value; Refresh();
            for (int i = 0; i < 40 && (recent_.halfHour != value.halfHour || recent_.hour != value.hour || recent_.observedMs != value.observedMs); ++i) co_await WaitLayout(100);
            if (recent_.halfHour != value.halfHour || recent_.hour != value.hour || recent_.observedMs != value.observedMs) throw std::runtime_error("Recent-only update did not arrive");
            DrawActivity(); co_await WaitLayout(150);
        };
        co_await activity({83,83,120000});
        if (Find<TextBlock>(L"PulseBadge").Text() != L"热身中") throw std::runtime_error("Warmup was presented as a complete rolling window");
        co_await SaveScreenshot(L"10-pulse-warmup.png");
        co_await activity({0,0,3600000});
        if (Find<TextBlock>(L"PulseHeadline").Text() != mouse::ActivityHeadline(mouse::PulseMood::Quiet)) throw std::runtime_error("Quiet activity kept a high-speed headline");
        co_await SaveScreenshot(L"11-pulse-quiet.png");
        co_await activity({6500,12000,3600000});
        if (Find<TextBlock>(L"PulseHeadline").Text() != mouse::ActivityHeadline(mouse::PulseMood::Marathon)) throw std::runtime_error("Hour milestone did not update independently of daily statistics");
        root_.RequestedTheme(ElementTheme::Dark); co_await WaitLayout(150); co_await SaveScreenshot(L"12-pulse-marathon.png");
        const auto dpi = GetDpiForWindow(hwnd_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 1100 * dpi / 96, 700 * dpi / 96, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        co_await WaitLayout(300); co_await SaveScreenshot(L"13-pulse-compact.png");
        auto scroll = Find<ScrollViewer>(L"MainScroll");
        scroll.ChangeView(nullptr, box_value(scroll.ScrollableHeight()).as<IReference<double>>(), nullptr, true);
        co_await WaitLayout(250); co_await SaveScreenshot(L"18-lifetime-group.png");
        scroll.ChangeView(nullptr, box_value(0.0).as<IReference<double>>(), nullptr, true); co_await WaitLayout(200);
        ShowSpeedTest(); co_await WaitLayout(350);
        if (!speedDialog_) throw std::runtime_error("Click speed dialog did not open");
        co_await SaveScreenshot(L"14-speed-ready.png");
        speedDialog_.FindName(L"TestDurationPicker").as<ComboBox>().SelectedIndex(1);
        SpeedTestPress(1);
        if (speedTest_.Status() != mouse::ClickSpeedTest::State::Ready) throw std::runtime_error("Wrong button started the speed test");
        SpeedTestPress(0); SpeedTestPress(0); co_await WaitLayout(100); SpeedTestPress(0);
        if (speedDialog_.FindName(L"TestDurationPicker").as<ComboBox>().IsEnabled()) throw std::runtime_error("Duration can change while testing");
        co_await SaveScreenshot(L"15-speed-running.png");
        co_await WaitLayout(3100);
        if (speedTest_.Status() != mouse::ClickSpeedTest::State::Finished || speedDialog_.FindName(L"TestApm").as<TextBlock>().Text() != L"60" || speedDialog_.FindName(L"TestCps").as<TextBlock>().Text() != L"1.00")
            throw std::runtime_error("Three-second left-button speed result is incorrect");
        SpeedTestPress(0);
        if (speedTest_.Clicks() != 3) throw std::runtime_error("Completed test accepted extra input");
        co_await SaveScreenshot(L"16-speed-result.png");
        speedDialog_.FindName(L"TestButtonPicker").as<ComboBox>().SelectedIndex(1);
        speedDialog_.FindName(L"TestDurationPicker").as<ComboBox>().SelectedIndex(0);
        SpeedTestPress(0); SpeedTestPress(1); co_await WaitLayout(1150);
        if (speedTest_.Clicks() != 1 || speedDialog_.FindName(L"TestApm").as<TextBlock>().Text() != L"60") throw std::runtime_error("One-second right-button test is incorrect");
        co_await SaveScreenshot(L"17-speed-right-result.png");
        speedDialog_.Hide(); co_await WaitLayout(250);
        co_await resume_background(); { mouse::SqliteStore store(database_, true); store.ExportCsv(verificationDirectory_ / L"export.csv", mouse::LocalDate(), mouse::LocalDate()); } co_await ui;
        const bool foregroundUnchanged = GetForegroundWindow() == originalForeground_;
        ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);
        if (!mouse::RequestDashboardActivation(hwnd_)) throw std::runtime_error("Dashboard activation request was not accepted");
        co_await WaitLayout(400);
        if (IsIconic(hwnd_) || !IsWindowVisible(hwnd_) || (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST)) throw std::runtime_error("Dashboard activation did not restore the window or left it topmost");
        const bool activatedForeground = GetForegroundWindow() == hwnd_;
        if (!activatedForeground) throw std::runtime_error("Dashboard restored without becoming the foreground window");
        struct CoverWindow { HWND handle{}; ~CoverWindow() { if (handle) DestroyWindow(handle); } } cover;
        RECT bounds{}; GetWindowRect(hwnd_, &bounds);
        cover.handle = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Mouse Click Stats · activation fixture", WS_OVERLAPPEDWINDOW,
            bounds.left + 50, bounds.top + 50, 460, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!cover.handle) throw std::runtime_error("Cannot create activation test cover");
        ShowWindow(cover.handle, SW_SHOWNORMAL); SetForegroundWindow(cover.handle); co_await WaitLayout(100);
        if (GetForegroundWindow() != cover.handle) throw std::runtime_error("Activation fixture did not obscure the dashboard");
        auto testBuild = verificationDirectory_;
        while (!testBuild.empty() && testBuild.filename() != L"build" && testBuild.parent_path() != testBuild) testBuild = testBuild.parent_path();
        const auto sender = testBuild / L"msvc" / L"mouse_tests.exe";
        if (!std::filesystem::is_regular_file(sender)) throw std::runtime_error("Cross-process activation test sender is missing");
        mouse::Launch(sender, L"--activate-dashboard-fixture " + std::to_wstring(reinterpret_cast<uintptr_t>(hwnd_)), true);
        for (int i = 0; i < 30 && (GetForegroundWindow() != hwnd_ || page_ != 2); ++i) co_await WaitLayout(100);
        if (GetForegroundWindow() != hwnd_ || page_ != 2) throw std::runtime_error("Cross-process activation/settings request failed to foreground the obscured dashboard");
        DestroyWindow(cover.handle); cover.handle = nullptr;
        ShowWindow(hwnd_, SW_MAXIMIZE); co_await WaitLayout(100);
        ShowWindow(hwnd_, SW_SHOWMINNOACTIVE); mouse::RequestDashboardActivation(hwnd_); co_await WaitLayout(300);
        if (!IsZoomed(hwnd_) || IsIconic(hwnd_) || GetForegroundWindow() != hwnd_ || (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST))
            throw std::runtime_error("Activation did not preserve maximized state without permanent topmost");
        std::ofstream report(verificationDirectory_ / L"report.json"); report << "{\"ok\":true,\"crossProcessActivationVerified\":true,\"maximizedRestoreVerified\":true,\"startupModesVerified\":true,\"activationRestoreVerified\":true,\"activatedForeground\":" << (activatedForeground ? "true" : "false") << ",\"secondaryMonitor\":" << (secondaryMonitor_ ? "true" : "false") << ",\"foregroundUnchanged\":" << (foregroundUnchanged ? "true" : "false")
            << ",\"totalClicks\":" << total << ",\"lifetimeClicks\":" << lifetimeBefore << ",\"overviewGroupingVerified\":true,\"lifetimeVerified\":true,\"speedTestVerified\":true,\"immersiveTitleBarVerified\":true,\"applications\":" << apps << ",\"iconsLoaded\":" << icons << ",\"buttonLegendItems\":5,\"emptyDayClicks\":" << empty << ",\"screenshots\":19,\"recentActivityVerified\":true,\"permissionStatesVerified\":true,\"taskbarCategoryVerified\":true,\"realizedContainers\":" << containers
            << ",\"unchangedListResets\":" << stableResets << ",\"skippedUnchanged\":" << skippedUnchanged_ << ",\"liveUpdateDelta\":" << liveDelta << ",\"liveRowUpdated\":" << (rowUpdated ? "true" : "false") << ",\"hiddenOverviewRenders\":" << hiddenRenders
            << ",\"dragUpdatesDeferred\":" << (draggingDeferred_ ? "true" : "false") << ",\"windowHost\":\"WinUI3\"" << ",\"worstApplyMs\":" << worstApplyMs_ << ",\"worstHeartbeatMs\":" << worstHeartbeatMs_ << "}";
        report.close();
        ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);
        if (!PostMessageW(hwnd_, mouse::kCollectorExitingMessage, 0, 0)) throw std::runtime_error("Cannot send collector exit notification");
        co_return;
    } catch (const hresult_error& error) { std::ofstream(verificationDirectory_ / L"error.txt") << to_string(error.message()); }
      catch (const std::exception& error) { std::ofstream(verificationDirectory_ / L"error.txt") << error.what(); }
    window_.Close();
}
