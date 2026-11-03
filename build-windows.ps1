[CmdletBinding()]
param(
    [string]$SdkPrefix = $env:REXGLUE_SDK_PREFIX,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 128)][int]$Jobs = 4,
    [switch]$CheckOnly,
    # A folder with llvm/, cmake-3.31.8-windows-x86_64/ and the Windows SDK
    # NuGet packages, laid out like the Naruto project's .tools.
    [string]$ToolsRoot
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'Run this script on Windows x64.' }

# Keep compiler temporaries beside the build instead of on the system drive.
$temp = Join-Path $PSScriptRoot 'out/tmp'
New-Item -ItemType Directory -Force $temp | Out-Null
$env:TEMP = $temp
$env:TMP = $temp

if ($ToolsRoot) {
    $tools = (Resolve-Path -LiteralPath $ToolsRoot).Path
    $sdkHeaders = Join-Path $tools 'windows-sdk-common/c/Include/10.0.26100.0'
    $sdkLibraries = Join-Path $tools 'windows-sdk-x64/c'
    $sdkBin = Join-Path $tools 'windows-sdk-common/c/bin/10.0.26100.0/x64'
    foreach ($file in @("$tools/llvm/bin/clang.exe",
                         "$tools/cmake-3.31.8-windows-x86_64/bin/cmake.exe",
                         "$sdkHeaders/um/Windows.h", "$sdkLibraries/um/x64/kernel32.lib",
                         "$sdkBin/rc.exe")) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "Local tools are incomplete: missing $file."
        }
    }
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio with C++ build tools is required.' }
        $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0 -or -not $vsPath) { throw 'Visual Studio C++ build tools were not found.' }
        Import-Module (Join-Path $vsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
        Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    }
    $env:PATH = "$tools/llvm/bin;$tools/cmake-3.31.8-windows-x86_64/bin;$sdkBin;$env:PATH"
    $env:INCLUDE = "$sdkHeaders/ucrt;$sdkHeaders/um;$sdkHeaders/shared;$sdkHeaders/winrt;$env:INCLUDE"
    $env:LIB = "$sdkLibraries/um/x64;$sdkLibraries/ucrt/x64;$env:LIB"
}

$problems = @()
foreach ($name in @('cmake', 'ninja', 'clang', 'clang++', 'link.exe', 'rc.exe')) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        $problems += "Missing $name on PATH. Use an x64 Visual Studio developer shell with LLVM 20+ installed."
    }
}
if (Get-Command clang -ErrorAction SilentlyContinue) {
    $versionText = (& clang --version | Out-String)
    if ($versionText -notmatch 'clang version (\d+)' -or [int]$Matches[1] -lt 20) {
        $problems += 'Install LLVM/Clang 20 or newer.'
    }
}
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
    $problems += 'Open x64 Developer PowerShell, or pass -ToolsRoot.'
}
$plugin = switch ($Configuration) {
    Debug { 'rexgpu-xenosd.dll' }
    Release { 'rexgpu-xenos.dll' }
    RelWithDebInfo { 'rexgpu-xenosrd.dll' }
}
if (-not $SdkPrefix -or -not (Test-Path -LiteralPath $SdkPrefix -PathType Container)) {
    $problems += 'Pass -SdkPrefix with the installed ReXGlue SDK prefix (see README).'
} else {
    foreach ($file in @('bin/rexglue.exe', "bin/$plugin")) {
        if (-not (Test-Path -LiteralPath (Join-Path $SdkPrefix $file) -PathType Leaf)) {
            $problems += "SDK is missing $file. Install the requested runtime configuration."
        }
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'assets/default.xex') -PathType Leaf)) {
    $problems += 'Missing assets/default.xex. Extract your own copy of the game into assets/ (README).'
}
if ($problems.Count) { throw ($problems -join [Environment]::NewLine) }
if ($CheckOnly) { Write-Host 'Windows build prerequisites passed.'; return }

$SdkPrefix = (Resolve-Path -LiteralPath $SdkPrefix).Path.Replace('\', '/')
$preset = 'win-amd64-' + $Configuration.ToLowerInvariant()
Push-Location $PSScriptRoot
try {
    & cmake --preset $preset "-DCMAKE_PREFIX_PATH=$SdkPrefix"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)." }
    & cmake --build --preset $preset --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "Windows build failed ($LASTEXITCODE)." }
    Write-Host "Built out/build/$preset/wwe2k15.exe. Launch with .\run-windows.ps1 -Configuration $Configuration"
} finally { Pop-Location }
