#requires -Version 7.0
param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('pluck-install-test-' + [Guid]::NewGuid().ToString('N'))))
$registryRoot = 'HKCU:\Software\Pluck.Tests\' + [IO.Path]::GetFileName($taskRoot)
$priorRoot = $env:PLUCK_FIXTURE_ROOT
New-Item -ItemType Directory -Path $taskRoot | Out-Null
$env:PLUCK_FIXTURE_ROOT = $taskRoot
$installed = Join-Path $taskRoot 'Local/Programs/Pluck/Pluck.exe'
$marker = Join-Path $taskRoot 'settings-opened.txt'
$failure = Join-Path $taskRoot 'last-error.txt'
$uninstallKey = Join-Path $registryRoot 'Software/Microsoft/Windows/CurrentVersion/Uninstall/Pluck'
$captureGate = $null
function Invoke-Fixture([string]$Path, [string[]]$Arguments) {
    $process = Start-Process -FilePath $Path -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(15000)) { throw 'The installer fixture did not exit.' }
    if ($process.ExitCode -ne 0) { throw "Fixture failed: $(Get-Content -LiteralPath $failure -ErrorAction SilentlyContinue)" }
}
function Wait-For([scriptblock]$Condition) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (& $Condition)) {
        if (Test-Path -LiteralPath $failure) { throw "Helper failed: $(Get-Content -LiteralPath $failure -Raw)" }
        if ([DateTime]::UtcNow -gt $deadline) { throw 'The installer helper timed out.' }
        Start-Sleep -Milliseconds 50
    }
}
try {
    Invoke-Fixture $Executable @('--install')
    Wait-For { Test-Path -LiteralPath $marker }
    if (-not (Test-Path -LiteralPath $installed) -or -not (Test-Path -LiteralPath $uninstallKey)) { throw 'Installation did not register the app.' }
    foreach ($name in 'Pluck.lnk', 'Pluck Settings.lnk') {
        if (-not (Test-Path -LiteralPath (Join-Path $taskRoot "Start Menu/Pluck/$name"))) { throw "Missing shortcut: $name" }
    }
    Write-Output 'PASS: per-user install, Windows registration, shortcuts, and settings restart in isolated folders'
    $keep = Join-Path (Split-Path -Parent $installed) 'keep.txt'
    Set-Content -LiteralPath $keep 'Retain user data.'
    Invoke-Fixture $installed @('--uninstall')
    Wait-For { -not (Test-Path -LiteralPath $uninstallKey) }
    if ((Test-Path -LiteralPath $installed) -or -not (Test-Path -LiteralPath $keep)) { throw 'Uninstall removed user data or left the app.' }
    Write-Output 'PASS: uninstall removes the app and registration and retains unknown files'
    Remove-Item -LiteralPath $marker
    Invoke-Fixture $Executable @('--install')
    Wait-For { Test-Path -LiteralPath $marker }
    $incoming = Join-Path $taskRoot 'next.exe'
    Invoke-Fixture $Executable @('--make-update', ('"' + $incoming + '"'))
    $before = (Get-FileHash -LiteralPath $installed).Hash
    Invoke-Fixture $installed @('--stage-update', ('"' + $incoming + '"'), '--bad-hash')
    Wait-For { Test-Path -LiteralPath $failure }
    if ((Get-Content -LiteralPath $failure -Raw) -notmatch 'changed after download') { throw 'The invalid hash was not checked.' }
    if ((Get-FileHash -LiteralPath $installed).Hash -ne $before) { throw 'An invalid update changed the app.' }
    Remove-Item -LiteralPath $failure
    Remove-Item -LiteralPath $marker
    $captureGate = [Threading.Mutex]::new($false, 'Local\Pluck.Screenshot')
    Invoke-Fixture $installed @('--stage-update', ('"' + $incoming + '"'))
    Wait-For {
        try { $gate = [Threading.Mutex]::OpenExisting('Local\Pluck.Maintenance'); $gate.Dispose(); return $true }
        catch [Threading.WaitHandleCannotBeOpenedException] { return $false }
    }
    if ((Get-FileHash -LiteralPath $installed).Hash -ne $before) { throw 'The update replaced an active capture.' }
    $captureGate.Dispose()
    $captureGate = $null
    Wait-For { Test-Path -LiteralPath $marker }
    if ((Get-FileHash -LiteralPath $installed).Hash -ne (Get-FileHash -LiteralPath $incoming).Hash) { throw 'The helper did not replace the app.' }
    $expected = [Diagnostics.FileVersionInfo]::GetVersionInfo($incoming)
    $expectedVersion = "$($expected.ProductMajorPart).$($expected.ProductMinorPart).$($expected.ProductBuildPart)"
    if ((Get-ItemProperty -LiteralPath $uninstallKey).DisplayVersion -ne $expectedVersion) { throw 'The Windows version was not updated.' }
    Write-Output 'PASS: invalid SHA-256 is rejected; update waits for capture, replaces the app, and reopens settings'
} finally {
    if ($captureGate) { $captureGate.Dispose() }
    $env:PLUCK_FIXTURE_ROOT = $priorRoot
    # Confirm all targets stay in this test root before process cleanup or recursive removal.
    $absoluteRoot = [IO.Path]::GetFullPath($taskRoot)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $absoluteRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetFileName($absoluteRoot).StartsWith('pluck-install-test-')) { throw 'Unsafe fixture cleanup path.' }
    Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($absoluteRoot + '\', [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object {
        if (-not $_.WaitForExit(3000)) { $_ | Stop-Process -Force }
    }
    if (Test-Path -LiteralPath $registryRoot) { Remove-Item -LiteralPath $registryRoot -Recurse -Force }
    Remove-Item -LiteralPath $absoluteRoot -Recurse -Force
}
