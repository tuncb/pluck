#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'scripts/version.ps1')
. (Join-Path $root 'scripts/verify-native.ps1')
$version = Get-PluckVersion
$temporary = Join-Path $root ('artifacts/package-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary -Force | Out-Null
$package = Join-Path $temporary 'app'
New-Item -ItemType Directory -Path $package -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'artifacts/win-x64/Pluck.exe') -Destination $package
function Assert-Rejected([scriptblock]$Action, [string]$Name) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw "Accepted invalid input: $Name" }
    Write-Output "PASS: $Name"
}
$packageScript = Join-Path $root 'scripts/package.ps1'
& $packageScript -PublishDirectory $package -ExpectedVersion $version -VerifyOnly
Set-Content -LiteralPath (Join-Path $package 'unexpected.txt') -Value 'not a runtime file'
Assert-Rejected { & $packageScript -PublishDirectory $package -VerifyOnly } 'Reject extra package files'
Remove-Item -LiteralPath (Join-Path $package 'unexpected.txt')
New-Item -ItemType Directory -Path (Join-Path $package 'extra') | Out-Null
Assert-Rejected { & $packageScript -PublishDirectory $package -VerifyOnly } 'Reject extra package directories'
Remove-Item -LiteralPath (Join-Path $package 'extra')
Assert-Rejected { & $packageScript -PublishDirectory $package -ExpectedVersion '65535.65535.65535' -VerifyOnly } 'Reject version mismatch'
Assert-Rejected { & $packageScript -PublishDirectory $package -DestinationDirectory (Join-Path $package 'nested') } 'Reject nested archive destination'
$fake = Join-Path $temporary 'fake.exe'
[IO.File]::WriteAllBytes($fake, [byte[]](0, 1, 2, 3))
Assert-Rejected { Get-PluckNativeImage -Path $fake } 'Reject invalid PE image'
$exe = Join-Path $package 'Pluck.exe'
$bytes = [IO.File]::ReadAllBytes($exe)
$pe = [BitConverter]::ToInt32($bytes, 60)
$bytes[$pe + 4] = 0x4c
$bytes[$pe + 5] = 0x01
[IO.File]::WriteAllBytes($fake, $bytes)
Assert-Rejected { Get-PluckNativeImage -Path $fake } 'Reject x86 executable'
$bytes = [IO.File]::ReadAllBytes($exe)
$bytes[$pe + 24 + 112 + 14 * 8] = 1
[IO.File]::WriteAllBytes($fake, $bytes)
Assert-Rejected { Get-PluckNativeImage -Path $fake } 'Reject managed executable'
foreach ($invalid in '01.0.0', '1.2', '1.2.3-beta', '65536.0.0') {
    [IO.File]::WriteAllText((Join-Path $temporary 'VERSION'), $invalid)
    Assert-Rejected { Get-PluckVersion -ProjectRoot $temporary } "Reject invalid version $invalid"
}
Remove-Item -LiteralPath $exe
Assert-Rejected { & $packageScript -PublishDirectory $package -VerifyOnly } 'Reject missing executable'
Write-Output 'Package failure checks passed.'
