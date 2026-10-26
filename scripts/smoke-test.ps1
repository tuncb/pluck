#requires -Version 7.0
param(
    [string]$Executable = (Join-Path $PSScriptRoot '../artifacts/win-x64/Pluck.exe'),
    [string]$TestExecutable = (Join-Path $PSScriptRoot '../build/Release/bin/pluck_desktop_tests.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../artifacts/smoke')
)
$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
$TestExecutable = [IO.Path]::GetFullPath($TestExecutable)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
Get-Process Pluck -ErrorAction SilentlyContinue | Where-Object Path -EQ $Executable | Stop-Process -Force
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
& $TestExecutable $Executable $OutputDirectory 2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory 'results.txt')
if ($LASTEXITCODE -ne 0) { throw 'Desktop tests failed. Use an unlocked Windows desktop.' }
