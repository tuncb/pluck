#requires -Version 7.0
$ErrorActionPreference = 'Stop'
Get-Process Pluck -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -and $_.Path.StartsWith($PSScriptRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
} | Stop-Process -Force
& (Join-Path $PSScriptRoot 'build.ps1')
# This starts the interactive screenshot selector requested by the user.
Start-Process -FilePath (Join-Path $PSScriptRoot 'build/Release/bin/Pluck.exe')
