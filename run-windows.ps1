[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$GameRoot,
    # Shader and pipeline caches; defaults to out/cache beside the build.
    [string]$CacheRoot,
    [ValidateSet('any', 'd3d12', 'vulkan')]
    [string]$Backend = 'vulkan',
    [ValidateRange(-1, 64)]
    [int]$VulkanDevice = -1,
    # xenos: GPU emulation. native: the work-in-progress native renderer
    # (docs/native-renderer.md), built when REXGLUE_SDK_SOURCE_DIR is set.
    # xenosprof: GPU emulation with debug symbols, for profiling only.
    [ValidateSet('xenos', 'native', 'xenosprof')]
    [string]$GpuPlugin = 'xenos',
    [switch]$Keyboard,
    [string[]]$GameArguments = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'Run this script on Windows.' }
if (-not $GameRoot) { $GameRoot = Join-Path $PSScriptRoot 'assets' }
if (-not $CacheRoot) { $CacheRoot = Join-Path $PSScriptRoot 'out/cache' }
New-Item -ItemType Directory -Force $CacheRoot | Out-Null
$preset = 'win-amd64-' + $Configuration.ToLowerInvariant()
$buildDirectory = Join-Path $PSScriptRoot "out/build/$preset"
$executable = Join-Path $buildDirectory 'wwe2k15.exe'
$plugin = if ($GpuPlugin -ne 'xenos') { "rexgpu-$GpuPlugin.dll" } else {
    switch ($Configuration) {
        Debug { 'rexgpu-xenosd.dll' }
        Release { 'rexgpu-xenos.dll' }
        RelWithDebInfo { 'rexgpu-xenosrd.dll' }
    }
}
foreach ($file in @($executable, (Join-Path $buildDirectory $plugin))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Missing $file. Build this configuration with build-windows.ps1 first."
    }
}
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$CacheRoot = (Resolve-Path -LiteralPath $CacheRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $GameRoot 'default.xex') -PathType Leaf)) {
    throw "Missing $GameRoot\default.xex. Extract your own copy of the game (README)."
}
Push-Location $buildDirectory
try {
    $logLevel = if ($Configuration -eq 'Debug') { 'debug' } else { 'info' }
    $nativeArguments = @("--game_data_root=$GameRoot", "--cache_root=$CacheRoot",
                         "--gpu_plugin=$GpuPlugin", "--log_level=$logLevel",
                         "--wwe2k15_graphics_backend=$Backend")
    if ($VulkanDevice -ge 0) { $nativeArguments += "--vulkan_device=$VulkanDevice" }
    if ($Keyboard) { $nativeArguments += '--mnk_mode=true' }
    $nativeArguments += $GameArguments
    # Windows GUI applications may return control without setting LASTEXITCODE.
    # Start the process directly and wait on its handle instead.
    $argumentLine = ($nativeArguments | ForEach-Object {
        '"' + (($_ -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
    }) -join ' '
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $executable
    $startInfo.Arguments = $argumentLine
    $startInfo.WorkingDirectory = $buildDirectory
    $startInfo.UseShellExecute = $false
    $gameProcess = [System.Diagnostics.Process]::Start($startInfo)
    try {
        $gameProcess.WaitForExit()
        $gameExitCode = $gameProcess.ExitCode
    } finally { $gameProcess.Dispose() }
    if ($gameExitCode -ne 0) {
        throw "wwe2k15 exited with code $gameExitCode. Check $buildDirectory/logs."
    }
} finally { Pop-Location }
