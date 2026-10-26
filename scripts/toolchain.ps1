#requires -Version 7.0
function Initialize-PluckToolchain {
    if (-not $IsWindows) { throw 'Build Pluck on Windows x64.' }
    # Keep a complete x64 developer environment for repeated builds in this shell.
    $requiredTools = 'cl.exe', 'rc.exe', 'cmake', 'ctest', 'ninja'
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and $env:VSCMD_ARG_HOST_ARCH -eq 'x64' -and
        $env:INCLUDE -and $env:LIB) {
        $missingTools = @($requiredTools | Where-Object { -not (Get-Command $_ -ErrorAction SilentlyContinue) })
        if ($missingTools.Count -eq 0) { return }
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = $null
    if (Test-Path -LiteralPath $vswhere) {
        $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0) { throw 'Could not find the Visual Studio C++ tools.' }
    }
    if ($visualStudio) {
        $env:PATH = (Split-Path -Parent $vswhere) + ';' + $env:PATH
        $developerCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
        $previousArgumentMode = $PSNativeCommandArgumentPassing
        try {
            $PSNativeCommandArgumentPassing = 'Legacy'
            $environment = & cmd.exe /d /c "`"$developerCommand`" -arch=x64 -host_arch=x64 >nul && set"
            if ($LASTEXITCODE -ne 0) { throw 'The C++ build environment could not start.' }
        } finally {
            $PSNativeCommandArgumentPassing = $previousArgumentMode
        }
        foreach ($line in $environment) {
            if ($line -match '^([^=]+)=(.*)$') {
                [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
            }
        }
        foreach ($tool in @(
            @{ Name = 'cmake'; Directory = 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin' },
            @{ Name = 'ninja'; Directory = 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja' }
        )) {
            if (-not (Get-Command $tool.Name -ErrorAction SilentlyContinue)) {
                $directory = Join-Path $visualStudio $tool.Directory
                if (Test-Path -LiteralPath $directory -PathType Container) { $env:PATH = "$directory;$env:PATH" }
            }
        }
    }
    foreach ($name in $requiredTools) {
        if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
            throw "Missing $name. Install Visual Studio 2022 or later with Desktop development with C++, a Windows SDK, and C++ CMake tools."
        }
    }
}

function Get-PluckBuildJobs {
    param([ValidateRange(0, 1024)][int]$Jobs = 0)
    if ($Jobs -gt 0) { return $Jobs }
    if ($env:CMAKE_BUILD_PARALLEL_LEVEL) {
        $value = 0
        if (-not [int]::TryParse($env:CMAKE_BUILD_PARALLEL_LEVEL, [ref]$value) -or $value -lt 1) {
            throw 'CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer.'
        }
        return $value
    }
    # Allow 2 GiB per compiler process. More than 16 jobs adds no useful work here.
    $memoryJobs = [math]::Max(1, [math]::Floor([GC]::GetGCMemoryInfo().TotalAvailableMemoryBytes / 2GB))
    return [int][math]::Min(16, [math]::Min([Environment]::ProcessorCount, $memoryJobs))
}
