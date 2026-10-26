#requires -Version 7.0
param(
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'Release',
    [switch]$Test,
    [switch]$Sanitize,
    [switch]$Publish,
    [switch]$Clean,
    [ValidateRange(0, 1024)][int]$Jobs = 0,
    [string]$Version
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts/version.ps1')
$appVersion = Get-PluckVersion -ExpectedVersion $Version
if ($Publish -and ($Sanitize -or $Configuration -ne 'Release')) {
    throw 'Publish the regular Release build.'
}
. (Join-Path $PSScriptRoot 'scripts/toolchain.ps1')
Initialize-PluckToolchain
$buildJobs = Get-PluckBuildJobs -Jobs $Jobs
$suffix = if ($Sanitize) { '-asan' } else { '' }
$buildDirectory = Join-Path $PSScriptRoot "build/$Configuration$suffix"
$sanitizer = if ($Sanitize) { 'ON' } else { 'OFF' }
& cmake -S $PSScriptRoot -B $buildDirectory -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" "-DPLUCK_SANITIZE=$sanitizer"
if ($LASTEXITCODE -ne 0) { throw 'Configure failed.' }
$executable = Join-Path $buildDirectory 'bin/Pluck.exe'
Get-Process Pluck -ErrorAction SilentlyContinue | Where-Object Path -EQ $executable | Stop-Process -Force
$arguments = @('--build', $buildDirectory, '--parallel', $buildJobs)
if ($Clean) { $arguments += '--clean-first' }
& cmake @arguments
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if ($Test) {
    $results = Join-Path $PSScriptRoot 'artifacts/test-results'
    New-Item -ItemType Directory -Path $results -Force | Out-Null
    & ctest --test-dir $buildDirectory --output-on-failure --no-tests=error --output-junit (Join-Path $results "core-$Configuration$suffix.xml")
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
if ($Publish) {
    $output = Join-Path $PSScriptRoot 'artifacts/win-x64'
    # Keep each publish clean and retain the previous package for inspection.
    $staging = Join-Path $PSScriptRoot ('artifacts/publish-' + [Guid]::NewGuid().ToString('N'))
    & cmake --install $buildDirectory --prefix $staging
    if ($LASTEXITCODE -ne 0) { throw 'Publish failed.' }
    & (Join-Path $PSScriptRoot 'scripts/package.ps1') -PublishDirectory $staging -ExpectedVersion $appVersion -VerifyOnly
    $artifactRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'artifacts'))
    foreach ($path in $output, $staging) {
        $absolute = [IO.Path]::GetFullPath($path)
        if (-not $absolute.StartsWith($artifactRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Publish path is outside artifacts: $absolute"
        }
        if ((Get-Item -LiteralPath $path -ErrorAction SilentlyContinue).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Publish path must not be a linked directory: $path"
        }
    }
    Get-Process Pluck -ErrorAction SilentlyContinue | Where-Object Path -EQ (Join-Path $output 'Pluck.exe') | Stop-Process -Force
    $previous = $null
    if (Test-Path -LiteralPath $output) {
        $previous = Join-Path $artifactRoot ('previous-publish-' + [Guid]::NewGuid().ToString('N'))
        [IO.Directory]::Move($output, $previous)
    }
    try { [IO.Directory]::Move($staging, $output) }
    catch {
        if ($previous -and -not (Test-Path -LiteralPath $output)) { [IO.Directory]::Move($previous, $output) }
        throw
    }
    Write-Output "Windows x64 build $appVersion`: $output/Pluck.exe"
}
