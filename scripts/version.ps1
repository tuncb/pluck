#requires -Version 7.0
function Get-PluckVersion {
    param([string]$ExpectedVersion, [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot))
    $version = (Get-Content -LiteralPath (Join-Path $ProjectRoot 'VERSION') -Raw).Trim()
    if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        throw 'VERSION must contain a version in the form X.X.X.'
    }
    foreach ($part in $version.Split('.')) {
        if ([long]$part -gt 65535) { throw 'Each Windows version field must be at most 65535.' }
    }
    if ($ExpectedVersion -and $ExpectedVersion -cne $version) {
        throw "The requested version is $ExpectedVersion. VERSION contains $version. Update VERSION before tagging the release."
    }
    return $version
}
