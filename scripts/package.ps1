#requires -Version 7.0
param(
    [string]$PublishDirectory = (Join-Path $PSScriptRoot '../artifacts/win-x64'),
    [string]$DestinationDirectory = (Join-Path $PSScriptRoot '../artifacts/packages'),
    [string]$ExpectedVersion,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
. (Join-Path $PSScriptRoot 'verify-native.ps1')
$version = Get-PluckVersion -ExpectedVersion $ExpectedVersion
$PublishDirectory = [IO.Path]::GetFullPath($PublishDirectory)
$DestinationDirectory = [IO.Path]::GetFullPath($DestinationDirectory)
if ($DestinationDirectory.Equals($PublishDirectory, [StringComparison]::OrdinalIgnoreCase) -or
    $DestinationDirectory.StartsWith($PublishDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The package directory must be outside the published application directory.'
}
$files = @(Get-ChildItem -LiteralPath $PublishDirectory -Force)
if ($files.Count -ne 1 -or $files[0].Name -cne 'Pluck.exe' -or $files[0].PSIsContainer) {
    throw 'The package must contain only Pluck.exe. Run ./build.ps1 -Test -Publish.'
}
$application = Join-Path $PublishDirectory 'Pluck.exe'
$actualVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($application).ProductVersion
if ($actualVersion -cne $version) { throw "The application version is $actualVersion. Expected $version." }
$image = Get-PluckNativeImage -Path $application
if ($VerifyOnly) {
    Write-Output "Verified native Windows x64 package $version (one executable)."
    return
}
New-Item -ItemType Directory -Path $DestinationDirectory -Force | Out-Null
$archivePath = Join-Path $DestinationDirectory 'pluck-windows-x64.zip'
$stream = [IO.File]::Open($archivePath, [IO.FileMode]::Create)
try {
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $application, 'Pluck.exe', [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    } finally { $archive.Dispose() }
} finally { $stream.Dispose() }
$archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    if ($archive.Entries.Count -ne 1 -or $archive.Entries[0].FullName -cne 'Pluck.exe') { throw 'ZIP contents differ.' }
    $stream = $archive.Entries[0].Open()
    try { $entryHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
    finally { $stream.Dispose() }
    if ($entryHash -ne (Get-FileHash -LiteralPath $application).Hash) { throw 'ZIP verification failed.' }
} finally { $archive.Dispose() }
$hash = (Get-FileHash -LiteralPath $archivePath).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$archivePath.sha256", "$hash  pluck-windows-x64.zip`n", [Text.UTF8Encoding]::new($false))
[ordered]@{
    version = $version; fileCount = 1; archiveBytes = (Get-Item -LiteralPath $archivePath).Length
    sha256 = $hash; executable = $image
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $DestinationDirectory 'package-verification.json') -Encoding utf8
Write-Output "Windows x64 package $version`: $archivePath"
