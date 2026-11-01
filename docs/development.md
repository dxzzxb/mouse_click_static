# 开发与构建

[返回首页](../README.md) · [使用指南](usage.md) · [更新记录](changelog.md)

## 环境与依赖

需要 Windows x64、Python 3.10+。采集核心使用 C++17 / Win32 / Raw Input，界面使用 C++20 / C++/WinRT / WinUI 3，项目代码使用静态 CRT。

构建脚本将微软 C++ 工具链和依赖下载到项目内的 `.tools`、`packages`，不安装 IDE、驱动或系统运行库。Visual Studio 工具包校验 SHA-256，NuGet 包按官方目录校验 SHA-512。

| 依赖 | 固定版本 |
| --- | --- |
| Windows SDK | `10.0.26100.9457` |
| C++/WinRT | `3.0.260818.1` |
| Windows App SDK | `2.5.1` |

Windows App SDK 各组件的版本号并不相同，具体清单及锁定原因见[运行库问题记录](known-issues.md)。bootstrap 将实际依赖写入 `.tools/dependencies.json`，构建据此选取头文件、元数据、DLL 和资源；C++/WinRT 投影按 SDK 版本分目录。

## 构建与打包

在仓库根目录执行：

```powershell
# 首次准备依赖、构建，并运行核心测试和隔离后台冒烟测试。
.\scripts\build.ps1

# 依赖已准备好时，直接增量构建。
python scripts/build.py

# 可选：生成依赖系统共享运行时的小体积版本。
python scripts/bootstrap.py --framework-dependent
python scripts/build.py --framework-dependent --skip-tests

# 将已有的当前版本发布目录打包为 ZIP。
python scripts/package.py
```

默认输出 `dist/MouseClickStats-<VERSION>-portable`，共享运行时版输出 `dist/MouseClickStats-<VERSION>-small`，版本号取自根目录 `VERSION`。构建会复制 README 和整个 `docs` 目录；打包脚本压缩已有发布目录，不重新构建。

只构建后台和核心测试时，可使用已有的 CMake / C++ 工具链：

```powershell
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --config Release
ctest --test-dir build/native -C Release --output-on-failure
```

## 发布布局

```text
MouseClickStats-0.3.5-portable/
  MouseClickStats.exe       # 用户启动入口
  runtime/                 # 界面、运行库、PRI 和完整语言资源
  README.md
  docs/
```

真正的界面 EXE 与依赖 DLL 位于同一目录，配套激活清单嵌入界面 EXE，资源保持相对位置。便携版携带 Windows App SDK 和 VC 运行库，从其他工作目录启动也应加载自身 `runtime` 内的文件，无需修改 PATH 或安装系统 Windows App Runtime。发布根目录不平铺 DLL 或语言目录。

推荐便携版。单独使用 small 包需要可正常初始化的匹配共享运行时；同级存在同版本的完整便携目录时，small 优先打开便携界面。Bootstrap 的最低版本约束可能选择系统中更新的运行时，仅降低编译依赖不能保证实际加载旧版。Windows 10 的拖动问题及版本对照见[已知问题](known-issues.md)，不通过删除或降级系统运行库处理。

## 实现约定

- 界面使用标准 WinUI 3 Window，保留原生 1 秒定时刷新、后台数据库读取和可见行增量更新，无焦点时继续刷新。移动期间合并延后数据更新，结束后排队应用；应用图标在后台提取并缓存。
- 兼容输入在独立线程使用低级鼠标监听补充按键，忽略移动、释放和注入标记，始终传递输入。两路逐键配对，Raw Input 优先，不对同一路连点去抖。
- 跨权限通信只放行固定、无指针的命令，不接收任意文件路径或执行内容。管理员采集器通过已有 Explorer 打开普通权限面板。
- 前台激活由界面线程完成，通过 `AllowSetForegroundWindow` 交接权限；用户切换到其他窗口后取消延后激活，不使用永久置顶、模拟按键或修改系统前台锁定设置。
- 管理员自启任务名为 `MouseClickStats-Admin-<当前账户 SID>`，执行发布目录的 `MouseClickStats.exe --background --take-over`。任务使用最高权限、当前账户登录触发器和无限执行时长，电池供电不停止采集；迁移时移除旧普通自启项，失败尝试恢复原配置。
- 自启配置助手先核对 UAC 后账户与发起账户一致，再修改设置，不启动采集或访问统计数据库。登录早于 Explorer 托盘就绪时，采集器保持运行，按秒重试图标注册。
- 清空数据由保存线程串行处理，两张表在同一事务内删除，失败回滚并保留未提交增量；旧队列不会在清空后重新写回，清空期间的新计数作为新统计保留。

## 验证方式

默认构建运行核心检查和隔离后台冒烟测试。真实界面验收使用：

```powershell
# 使用隔离数据库检查界面、图标、窗口恢复与激活，不发送鼠标键盘输入。
python scripts/verify-ui.py --slow-read

# 加入 1205 个来源、350 ms 后台读取延迟和无焦点自动刷新检查。
python scripts/verify-ui.py --stress --slow-read
```

核心检查覆盖五键、连点、换日、Unicode、滚轮、CSV、事务回滚、写入重试、退出排空、进程权限、清空顺序与回滚、任务栏与窗口归属、鼠标捕获、短时窗口及点评阈值，以及全历史累计、大整数饱和和手速测试边界。

界面压力验收检查实时 / 日期 / 累计分组、历史日期下累计保持不变、累计实时增长、左右键测试、权限横幅、任务栏详情和实时活动状态。数据库写入后不主动刷新，要求窗口在无焦点时自动发现 4 次增长，并核对可见行、列表重设与隐藏页面重绘次数。

## 既有验证记录

以下为原文档保留的版本验收记录，整理文档时未重新运行程序测试。

| 版本 | 核心检查 | 其他记录 |
| --- | --- | --- |
| 0.3.1 | 75 项 | 新部署布局与隔离界面验收；从其他工作目录启动，核对应用内运行库加载路径 |
| 0.3.3 | 110 项 | 后台冒烟测试、1205 来源界面验收，最小化面板响应退出通知 |
| 0.3.4 | 141 项 | 后台冒烟测试、1205 来源与延迟读取界面验收；18 张截图、CSV 和 JSON |
| 0.3.5 | 156 项 | 后台冒烟测试、1205 来源界面验收；19 张截图、CSV 和 JSON |

0.3.5 的输出位于 `build/ui-verification-v0.3.5-stress`。该轮还检查三种自启模式、未应用选择保留、跨进程激活、最小化恢复、最大化状态保留及无永久置顶。管理员任务 XML 使用 Windows `TASK_VALIDATE_ONLY` 验证，并核对前后任务定义未改变；错误账户和未授权配置助手在修改前拒绝执行。

### 验证边界

- 自动测试使用构建目录中的隔离数据库，不操作日常统计数据。
- 上述 0.3.5 验收没有替用户注册管理员自启任务，也没有重启电脑；实际 UAC 设置与下次登录运行需要在用户启用后验证。
- 本机已有系统运行库，便携部署验证不等同于全新无运行时虚拟机验收。
- 全屏对局、特定反作弊、睡眠 / 设备恢复及 1000 / 4000 / 8000 Hz 鼠标下的游戏帧时间，需要在对应环境实测。
