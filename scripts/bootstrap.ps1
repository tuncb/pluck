#requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'toolchain.ps1')
Initialize-PluckToolchain
Get-Command cl.exe, rc.exe, cmake, ctest, ninja | Select-Object Name, Source
Write-Output 'Native build tools are ready. Run ./build.ps1 -Test -Publish.'
