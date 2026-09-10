#requires -Version 5.1
[CmdletBinding()]
param([string]$GameDirectory, [switch]$CheckOnly, [switch]$Yes)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-RegularFile([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "不是普通文件，已停止：$Path"
    }
    if ($item.Length -eq 0) { throw "文件为空：$Path" }
}
function Get-Hash([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}
function Assert-GameStopped {
    if (Get-Process -Name 'ffxiv_dx11', 'ffxiv' -ErrorAction SilentlyContinue) {
        throw '请先完全退出所有 FF14 游戏窗口，再运行安装程序。'
    }
}

try {
    if (-not [Environment]::Is64BitProcess) { throw '请使用 64 位 Windows PowerShell 运行安装程序。' }
    $package = $PSScriptRoot
    $manifest = Get-Content -LiteralPath (Join-Path $package 'package-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    # Fixed allowlist: a manifest must never add arbitrary target paths.
    $files = @('winmm.dll', 'OptiScaler.ini', 'amd_presr_perf.ini')
    $manifestNames = @($manifest.files | ForEach-Object { $_.name })
    if ($manifestNames.Count -ne 3 -or (Compare-Object $files $manifestNames)) { throw '安装包文件清单无效。' }
    foreach ($entry in $manifest.files) {
        $source = Join-Path (Join-Path $package 'payload') $entry.name
        Assert-RegularFile $source
        if ((Get-Hash $source) -ne $entry.sha256) { throw "安装包损坏或文件被修改：$($entry.name)。请重新完整解压。" }
    }

    if (-not $GameDirectory) {
        if ($CheckOnly -or $Yes) { throw '命令行模式需要指定 -GameDirectory。' }
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object Windows.Forms.OpenFileDialog
        $dialog.Title = '选择 FF14 游戏目录中的 ffxiv_dx11.exe'
        $dialog.Filter = 'FF14 DX11 游戏程序 (ffxiv_dx11.exe)|ffxiv_dx11.exe'
        try {
            if ($dialog.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { Write-Host '已取消，未修改文件。'; exit 0 }
            $GameDirectory = Split-Path -Parent $dialog.FileName
        } finally { $dialog.Dispose() }
    }
    $GameDirectory = $GameDirectory.Trim().Trim('"')
    $selected = Get-Item -LiteralPath $GameDirectory -Force
    if (-not $selected.PSIsContainer) {
        if ($selected.Name -ine 'ffxiv_dx11.exe') { throw '请选择游戏目录或 ffxiv_dx11.exe。' }
        $selected = $selected.Directory
    }
    $target = $selected.FullName
    # Refuse redirected paths rather than installing outside the selected directory.
    $ancestor = $selected
    while ($null -ne $ancestor) {
        if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '游戏路径包含目录链接，请选择其实际存放目录。' }
        $ancestor = $ancestor.Parent
    }
    Assert-RegularFile (Join-Path $target 'ffxiv_dx11.exe')
    Assert-GameStopped

    $missing = New-Object 'System.Collections.Generic.List[string]'
    foreach ($entry in $manifest.dependencies) {
        $relative = [string]$entry.name
        if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)') { throw '依赖清单路径无效。' }
        $path = Join-Path $target $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $missing.Add($relative); continue }
        Assert-RegularFile $path
        if ($entry.sha256 -and (Get-Hash $path) -ne $entry.sha256) { throw "依赖版本不匹配或文件损坏：$relative。请使用说明中指定的原版组件包。" }
    }
    if ($missing.Count) { throw ("缺少运行依赖，请先按说明安装原版组件，再运行此补丁：`n  " + ($missing -join "`n  ")) }
    foreach ($name in $files) {
        $path = Join-Path $target $name
        if (Test-Path -LiteralPath $path) {
            $item = Get-Item -LiteralPath $path -Force
            if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "目标文件被目录或链接占用：$name" }
            if ($item.IsReadOnly) { throw "目标文件为只读，请先解除只读属性：$name" }
        }
    }
    $hipDirs = @($target) + @($env:Path -split ';')
    if ($env:HIP_PATH) { $hipDirs += (Join-Path $env:HIP_PATH 'bin') }
    $hipFound = @($hipDirs | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'amdhip64_7.dll') -PathType Leaf) }).Count -gt 0
    if (-not $hipFound) { Write-Warning '未在当前环境检测到 HIP 7。游戏运行仍需要 HIP 7；安装 SDK 后请重启游戏启动器。此检查不验证显卡兼容性。' }
    Write-Host "`n目标目录：$target"
    Write-Host "版本：$($manifest.version)"
    Write-Host '将备份并替换 winmm.dll、OptiScaler.ini、amd_presr_perf.ini。'
    Write-Host '默认：FSR 1.3 倍、DLSS5 NR 75%、单轮；插帧关闭，可在菜单开启。'
    Write-Host '关闭本优化版的主日志、AMD 桥接日志和性能 CSV；第三方组件仍可能单独写日志。'
    if ($CheckOnly) { Write-Host '检查通过；未修改游戏目录。'; exit 0 }
    if (-not $Yes) {
        if ((Read-Host '这会重置现有插件配置。输入 Y 安装，其他输入取消') -ine 'Y') { Write-Host '已取消，未修改文件。'; exit 0 }
    }
    Assert-GameStopped
    $backup = Join-Path $target ('OptiScaler-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
    [IO.Directory]::CreateDirectory($backup) | Out-Null
    $stage = Join-Path $backup 'new-files'
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    $original = @{}
    foreach ($entry in $manifest.files) {
        $name = [string]$entry.name
        $destination = Join-Path $target $name
        $exists = Test-Path -LiteralPath $destination -PathType Leaf
        $original[$name] = $exists
        if ($exists) {
            Copy-Item -LiteralPath $destination -Destination (Join-Path $backup $name)
            if ((Get-Hash $destination) -ne (Get-Hash (Join-Path $backup $name))) { throw "备份校验失败，未替换文件：$name" }
        }
        Copy-Item -LiteralPath (Join-Path (Join-Path $package 'payload') $name) -Destination (Join-Path $stage $name)
        if ((Get-Hash (Join-Path $stage $name)) -ne $entry.sha256) { throw "暂存校验失败，未替换文件：$name" }
    }
    $original | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'original-files.json') -Encoding UTF8
    $changed = New-Object 'System.Collections.Generic.List[string]'
    try {
        Assert-GameStopped
        foreach ($name in $files) {
            $destination = Join-Path $target $name
            if ($original[$name]) { [IO.File]::Replace((Join-Path $stage $name), $destination, [NullString]::Value) }
            else { [IO.File]::Move((Join-Path $stage $name), $destination) }
            $changed.Add($name)
        }
        foreach ($entry in $manifest.files) {
            if ((Get-Hash (Join-Path $target $entry.name)) -ne $entry.sha256) { throw "安装后校验失败：$($entry.name)" }
        }
    } catch {
        $failure = $_.Exception.Message
        $rollbackErrors = New-Object 'System.Collections.Generic.List[string]'
        foreach ($name in $changed) {
            try {
                $destination = Join-Path $target $name
                if ($original[$name]) { [IO.File]::Copy((Join-Path $backup $name), $destination, $true) }
                else { Remove-Item -LiteralPath $destination -Force }
            } catch { $rollbackErrors.Add($name) }
        }
        if ($rollbackErrors.Count) { throw "安装失败：$failure。部分文件需从备份手动恢复：$($rollbackErrors -join ', ')。备份：$backup" }
        throw "安装失败，已撤销本次文件替换：$failure。备份保留在：$backup"
    }
    Write-Host "`n安装完成，文件校验通过。原文件备份：$backup" -ForegroundColor Green
    Write-Host '请在游戏画面设置中选择 DLSS，进入游戏后按 Insert 打开中文菜单。'
    Write-Host '改变超分倍率后需保存设置并重启游戏。已有日志不会被删除。'
    exit 0
} catch {
    Write-Host ("`n安装未完成：" + $_.Exception.Message) -ForegroundColor Red
    Write-Host '如提示访问被拒绝，请确认游戏已退出且目录可写；必要时以管理员身份运行 INSTALL.bat。'
    exit 1
}
