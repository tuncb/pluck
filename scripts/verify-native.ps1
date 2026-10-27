#requires -Version 7.0
function Get-PluckNativeImage {
    param([Parameter(Mandatory)][string]$Path)
    $bytes = [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($Path))
    function Read-U16([int]$offset) {
        if ($offset -lt 0 -or $offset + 2 -gt $bytes.Length) { throw "Invalid PE image: $Path" }
        [BitConverter]::ToUInt16($bytes, $offset)
    }
    function Read-U32([int]$offset) {
        if ($offset -lt 0 -or $offset + 4 -gt $bytes.Length) { throw "Invalid PE image: $Path" }
        [BitConverter]::ToUInt32($bytes, $offset)
    }
    if ((Read-U16 0) -ne 0x5a4d) { throw "Missing executable header: $Path" }
    $pe = [int](Read-U32 60)
    if ((Read-U32 $pe) -ne 0x4550 -or (Read-U16 ($pe + 4)) -ne 0x8664) {
        throw "The executable must be Windows x64: $Path"
    }
    $optional = $pe + 24
    if ((Read-U16 $optional) -ne 0x20b) { throw "The executable must use PE32+: $Path" }
    $directories = $optional + 112
    if ((Read-U32 ($directories + 14 * 8)) -ne 0 -or (Read-U32 ($directories + 14 * 8 + 4)) -ne 0) {
        throw "The executable contains a .NET CLR header: $Path"
    }
    $sections = $optional + (Read-U16 ($pe + 20))
    $sectionCount = Read-U16 ($pe + 6)
    function Convert-Rva([uint32]$rva) {
        for ($i = 0; $i -lt $sectionCount; $i++) {
            $section = $sections + 40 * $i
            $start = Read-U32 ($section + 12)
            $size = [Math]::Max((Read-U32 ($section + 8)), (Read-U32 ($section + 16)))
            if ($rva -ge $start -and $rva - $start -lt $size) {
                return [int]((Read-U32 ($section + 20)) + $rva - $start)
            }
        }
        throw "Invalid PE address in $Path"
    }
    $imports = [Collections.Generic.List[string]]::new()
    $delayImports = [Collections.Generic.List[string]]::new()
    function Read-ImportName([uint32]$rva) {
        $start = Convert-Rva $rva
        $end = $start
        while ($end -lt $bytes.Length -and $bytes[$end] -ne 0 -and $end - $start -lt 512) { $end++ }
        if ($end -ge $bytes.Length -or $end - $start -ge 512) { throw "Invalid PE import name in $Path" }
        [Text.Encoding]::ASCII.GetString($bytes, $start, $end - $start)
    }
    $importRva = Read-U32 ($directories + 8)
    if ($importRva) {
        $descriptor = Convert-Rva $importRva
        for ($i = 0; $i -lt 256; $i++) {
            $nameRva = Read-U32 ($descriptor + 20 * $i + 12)
            if (-not $nameRva) { break }
            $imports.Add((Read-ImportName $nameRva))
        }
    }
    $delayRva = Read-U32 ($directories + 13 * 8)
    if ($delayRva) {
        $descriptor = Convert-Rva $delayRva
        for ($i = 0; $i -lt 256; $i++) {
            $entry = $descriptor + 32 * $i
            $nameRva = Read-U32 ($entry + 4)
            if (-not $nameRva) { break }
            if ((Read-U32 $entry) -ne 1) { throw "Invalid PE delayed import in $Path" }
            $delayImports.Add((Read-ImportName $nameRva))
        }
    }
    foreach ($dependency in @($imports.ToArray()) + @($delayImports.ToArray())) {
        if ($dependency -match '^(mscoree|coreclr|hostfxr|hostpolicy|vcruntime\d.*|msvcp\d.*|concrt\d.*|clang_rt.*|libgcc_s.*|libstdc\+\+.*|libwinpthread.*)\.dll$') {
            throw "The executable requires an external runtime: $dependency ($Path)"
        }
    }
    [pscustomobject]@{ Path = [IO.Path]::GetFullPath($Path); Architecture = 'x64'; ClrHeader = $false; Imports = $imports.ToArray(); DelayImports = $delayImports.ToArray() }
}
