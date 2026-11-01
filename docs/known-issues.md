# 运行库问题记录

## 0.3.5：管理员登录自启与托盘前台激活

之前手动 UAC 提权只替换当前采集进程，登录自启仍是 HKCU Run 普通权限入口，因此重启后权限恢复普通。新版将自启独立为关闭 / 普通 / 管理员三种模式，选择后应用。管理员模式以当前账户交互令牌、最高权限和登录触发器注册任务，不保存密码；普通入口与管理员任务相互迁移，取消 UAC 不修改配置，失败尝试回滚。更换发布目录后需重新应用以更新任务路径。账户与权限规则依据：[Task Scheduler 安全上下文](https://learn.microsoft.com/en-us/windows/win32/taskschd/security-contexts-for-running-tasks)。

托盘唤出改为向界面进程明确移交前台权限，再发送固定无指针消息，由界面线程恢复和激活。冷启动通过子进程运行前授权、Explorer COM 授权和短时启动交接处理；用户切到其他窗口后取消延后的激活，不改永久置顶状态。依据：[SetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow)。登录时托盘尚未就绪，采集器继续工作并重试图标注册。

156 项核心检查、隔离后台冒烟测试、19 张截图及 1205 来源界面压力验收通过；实际桌面验证被遮挡界面的跨进程前台激活、最小化恢复和最大化状态保留。任务定义仅使用 `TASK_VALIDATE_ONLY`，核对前后未注册或改变任务；没有替用户保存真实自启配置，尚未执行 UAC 安装或电脑重启验证。

## 0.3.3：普通窗口切换归属与面板退出

0.3.2 仅在任务栏分支使用输入位置命中的窗口，其余输入仍回到前台窗口。因此点击另一个窗口时，如果前台窗口正处于失活/激活交接，空句柄会直接成为“未知应用”。[GetForegroundWindow 官方说明](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getforegroundwindow)明确允许失活期间返回空句柄。

0.3.3 按键优先解析命中窗口的根窗口，鼠标捕获优先于命中窗口；滚轮优先按前台窗口，无法识别时回退命中窗口。主、副屏任务栏仍单独归类。测试覆盖空前台、普通子窗口、任务栏、捕获和无窗口可用的情况，不把旧未知记录重新猜给某个应用。输入排队期间窗口消失、覆盖层和无法查询进程等情况仍可能留下未知或归属误差。

托盘退出在保存结束后向面板发送固定、无指针的退出通知；面板最小化时也响应。采集器已经连接过、随后连续离线 8 秒时另有兜底关闭。提权接棒使用独立退出标志，保留面板；初次尚未连接不会触发关闭。110 项核心检查和 1205 来源的界面验收通过，后者还验证了最小化面板通过退出通知关闭。自动验收不向真实桌面发送鼠标或键盘输入，实际窗口切换手感仍需结合使用观察。

## Windows App SDK 2.6.4：Windows 10 窗口拖动严重滞后

记录日期：2026-11-01。这里的版本号指 `Microsoft.WindowsAppSDK` NuGet 总包及其对应运行组件。

### 现象与验证

- 本机 Windows 10 Pro 22H2，系统版本 19045.6466。
- 使用 2.6.4 时，拖动整个统计窗口会明显落后于鼠标；同屏资源管理器跟手。
- 降低数据刷新负担、增加 Win32 窗口承载、拖动期间启用逐帧回调，都未通过用户的拖动验收。移动消息最慢约 0.67 ms，不能用消息耗时或 UI 定时回调指标代替实际拖动体验。
- 界面代码保持一致、应用自带运行库改为 2.5.1 后，用户确认拖动恢复跟手。已核对进程加载的是发布目录内的 DLL，排除实际仍使用系统 2.6.4 的情况。
- 0.3.0 撤回额外 Win32 窗口承载与逐帧回调，恢复标准 WinUI 3 Window，继续锁定 2.5.1；无焦点自动刷新和界面验收通过。

上游也有相同版本在 Windows 10 上拖动/缩放异常的报告，提交者说明 2.5.1 正常：
[microsoft/microsoft-ui-xaml #12471](https://github.com/microsoft/microsoft-ui-xaml/issues/12471)。
本项目的结论来自本机版本对照与用户验收；尚未定位到运行库内部的具体源码提交。

### 当前固定依赖

Windows App SDK 2.5.1 的组件版本并不相同，下列子包均为当前有效依赖，清理时需要保留：

| NuGet 包 | 版本 |
| --- | --- |
| Microsoft.WindowsAppSDK | 2.5.1 |
| Microsoft.WindowsAppSDK.Base | 2.0.4 |
| Microsoft.WindowsAppSDK.Foundation | 2.3.12 |
| Microsoft.WindowsAppSDK.InteractiveExperiences | 2.1.9 |
| Microsoft.WindowsAppSDK.WinUI | 2.3.9 |
| Microsoft.WindowsAppSDK.DWrite | 2.1.0 |
| Microsoft.WindowsAppSDK.Runtime（共享运行时开发包） | 2.5.1 |

该组合中的 `Microsoft.UI.Xaml.dll` 文件版本为 `3.2.0.2511`，与 SDK 总包版本采用不同编号。
完整构建清单由 bootstrap 写入 `.tools/dependencies.json`；构建脚本据此选取头文件、元数据、资源和 DLL。C++/WinRT 投影按 SDK 版本分目录。

### 后续维护

1. 默认使用应用自带 2.5.1 运行库的便携版。共享运行时 Bootstrap 的最低版本约束可能选中系统更高版本，不能单靠降低编译依赖保证实际运行版本。
2. 升级 SDK 前，在 Windows 10 上人工对比系统标题栏拖动，并验证切到其他应用时的统计刷新、主题、缩放、图标与清空功能。
3. 核对实际加载的 DLL 路径和文件版本，再判断版本对照结果。不通过删除或降级系统运行库来修复本项目。
4. 已解压且保留恢复标记的下载压缩包可以删除；保留锁定清单、当前解压依赖、编译工具链和当前版本的投影即可继续构建。

普通权限采集器漏记管理员运行的 LOL，已通过按需提升采集器权限解决；无焦点列表不更新，已通过原生定时刷新与后台读取解决。这两项具有独立的处理和验证记录。

## 0.3.0：混用两个发布目录时，便携入口被转到共享界面

先启动 small 后台，再运行 portable 主入口时，旧入口会通知既有后台打开界面。既有后台按自己的目录选择 small 界面，因此便携主入口也可能显示共享运行时错误。

0.3.1 改为入口选择自己安装目录的界面；共享界面在同级存在同版本便携包时，优先交给本地便携界面启动，并在获取界面单实例锁前完成交接。共享运行时 API 的调用顺序也调整为先初始化 COM。

本机独立共享模式仍返回 `0x80670016`（`STATEREPOSITORY_E_DEPENDENCY_NOT_RESOLVED`）。当前用户虽有已注册运行时，包依赖条件仍无法解析；不将此错误统一解释为“完全没安装运行时”，也不自动修复或改动系统包。独立 small 包仍需可正常初始化的共享运行时。

## 0.3.1 便携部署布局与验证边界

发布根目录只保留一个 `MouseClickStats.exe` 入口。真正的界面 EXE、配套激活清单对应的 DLL、VC 文件、PRI 和完整语言资源均放在 `runtime` 内，内部界面与依赖仍处于同一目录。GUI 启动后台、自启和按需提权使用安装根目录中的采集器。

已经在 Windows 10 22H2 上从 `C:\Windows\System32` 工作目录启动根入口，确认所有非 Windows 系统模块均加载自应用的 `runtime`，没有加载 WindowsApps 中的 WinUI；也验证了从 small 根入口冷启动会转到同版本便携界面。75 项核心检查和新布局的隔离界面验收通过。

本机安装过系统运行时，且没有可直接使用的 Windows Sandbox，以上结果不等同于全新无运行时虚拟机验收。免安装采用自包含部署：带齐整个便携目录即可使用，不能只复制根 EXE。依据：[微软自包含部署说明](https://learn.microsoft.com/windows/apps/package-and-deploy/self-contained-deploy/deploy-self-contained-apps)、[DLL 搜索顺序](https://learn.microsoft.com/windows/win32/dlls/dynamic-link-library-search-order)、[Bootstrap 初始化要求](https://learn.microsoft.com/windows/windows-app-sdk/api/win32/mddbootstrap/nf-mddbootstrap-mddbootstrapinitialize2)。
