# build_fc.ps1 - firmware build entry for fit_coach (same workflow as
# astock_quote / weiqi-quest).
#
# Keep this file PURE ASCII: Windows PowerShell 5.1 reads BOM-less .ps1 files
# with the ANSI codepage (GBK on zh-CN systems); UTF-8 Chinese comments would
# break parsing.
#
# ninja's .ninja_deps caches GSP-generated file paths in /D:/ style, and
# incremental builds then fail with FindFirstFileExA. Count /D:/ entries first
# and only remove .ninja_deps when the count is non-zero (keeps incremental
# builds fast). Never remove .ninja_log.
param(
    [switch]$Clean   # also remove the whole build dir (config reruns too)
)

$ErrorActionPreference = "Stop"
$workspace = "D:\esp-mo\esp-mosaico-vibe-main"
$project = "projects/fit_coach"
$build = "$workspace\projects\fit_coach\build"
$idf = "D:\esp-mo\esp-idf"
$script = "$workspace\submodule\esp-mosaico-utils\mosaico-tools\skills\idf-low-noise-build\scripts\idf_low_noise_build.py"

if ($Clean -and (Test-Path $build)) {
    Write-Host "[build] clean: removing $build"
    Remove-Item $build -Recurse -Force
}

if (Test-Path "$build\.ninja_deps") {
    $bad = (Select-String -Path "$build\.ninja_deps" -Pattern '/D:/' -AllMatches |
            Measure-Object).Count
    if ($bad -gt 0) {
        Write-Host "[build] removing stale .ninja_deps ($bad /D:/ entries)"
        Remove-Item "$build\.ninja_deps" -Force
    } else {
        Write-Host "[build] .ninja_deps clean ($bad /D:/ entries), keeping incremental"
    }
}

Write-Host "[build] building $project ..."
Push-Location $workspace
try {
    $env:IDF_PATH = $idf
    # idf_tools.py refuses to run when MSYSTEM is set ("MSys/Mingw is not
    # supported"). Launched from Git Bash, the MSYS2 runtime re-injects the
    # variable into child processes (env -u does not stick), so scrub it here
    # inside the PowerShell process before spawning python.
    Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
    # Verified recipe: no ccache (any non-empty IDF_CCACHE_ENABLE means True)
    # and serial compilation. cc1 OOM is caused by transient system commit
    # memory pressure; -j1 is the most stable when free RAM is low.
    Remove-Item Env:IDF_CCACHE_ENABLE -ErrorAction SilentlyContinue
    $env:IDF_PY_BUILD_JOBS = "1"
    python $script --project $project build
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}
Write-Host "[build] OK"
