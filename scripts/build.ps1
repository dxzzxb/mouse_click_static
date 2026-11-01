param(
    [switch]$Bootstrap,
    [switch]$FrameworkDependent,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    $needsBootstrap = $Bootstrap -or -not (Test-Path -LiteralPath '.tools\dependencies.json')
    if (-not $needsBootstrap -and $FrameworkDependent) {
        $dependencyState = Get-Content -LiteralPath '.tools\dependencies.json' -Raw | ConvertFrom-Json
        $runtimeStamp = Join-Path 'packages' ('Microsoft.WindowsAppSDK.Runtime.' + $dependencyState.windowsAppSdk + '\.restored')
        $needsBootstrap = -not (Test-Path -LiteralPath $runtimeStamp)
    }
    if ($needsBootstrap) {
        Write-Host '下载并校验项目内的微软 C++ 工具链、Windows SDK 与 WinUI 依赖（不安装系统组件）…'
        $bootstrapArguments = @('scripts/bootstrap.py')
        if ($FrameworkDependent) { $bootstrapArguments += '--framework-dependent' }
        python @bootstrapArguments
        if ($LASTEXITCODE -ne 0) { throw '依赖准备失败' }
    }
    $buildArguments = @('scripts/build.py')
    if ($FrameworkDependent) { $buildArguments += '--framework-dependent' }
    if ($SkipTests) { $buildArguments += '--skip-tests' }
    Write-Host '编译 C++ 采集器、WinUI 3 面板与测试，并生成发布目录…'
    python @buildArguments
    if ($LASTEXITCODE -ne 0) { throw '构建或测试失败' }
} finally {
    Pop-Location
}

