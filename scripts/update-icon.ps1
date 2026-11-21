#requires -Version 7.0
# Generate the Windows icon from the SVG. Normal builds use the ICO directly.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$iconSource = Join-Path $projectRoot 'assets/Pluck.svg'
$iconOutput = Join-Path $projectRoot 'assets/Pluck.ico'
$imageMagick = Get-Command magick -ErrorAction SilentlyContinue
if (-not $imageMagick) { throw 'Install ImageMagick with SVG support to generate the app icon.' }

# Render each size as a stripped 32-bit PNG and pack into a standard PNG-compressed ICO.
# Windows Vista and later load PNG-compressed ICO entries natively, keeping Pluck.exe small.
$sizes = @(256, 128, 96, 64, 48, 40, 32, 24, 20, 16)
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('pluck-icon-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary -Force | Out-Null
try {
    $entries = [Collections.Generic.List[object]]::new()
    foreach ($size in $sizes) {
        $pngPath = Join-Path $temporary "icon-$size.png"
        & $imageMagick.Source -background none -density 1536 $iconSource -resize "${size}x${size}" -depth 8 -strip "PNG32:$pngPath"
        if ($LASTEXITCODE -ne 0) { throw "App icon generation failed at ${size}x${size}." }
        $entries.Add([pscustomobject]@{ Size = $size; Bytes = [IO.File]::ReadAllBytes($pngPath) })
    }
    $stream = [IO.MemoryStream]::new()
    try {
        $writer = [IO.BinaryWriter]::new($stream)
        try {
            $writer.Write([uint16]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]$entries.Count)
            $offset = [uint32](6 + 16 * $entries.Count)
            foreach ($entry in $entries) {
                $dimension = if ($entry.Size -ge 256) { [byte]0 } else { [byte]$entry.Size }
                $writer.Write($dimension)
                $writer.Write($dimension)
                $writer.Write([byte]0)
                $writer.Write([byte]0)
                $writer.Write([uint16]1)
                $writer.Write([uint16]32)
                $writer.Write([uint32]$entry.Bytes.Length)
                $writer.Write($offset)
                $offset += [uint32]$entry.Bytes.Length
            }
            foreach ($entry in $entries) {
                $writer.Write($entry.Bytes)
            }
            $writer.Flush()
            [IO.File]::WriteAllBytes($iconOutput, $stream.ToArray())
        } finally {
            $writer.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Output "Windows icon: $iconOutput ($((Get-Item -LiteralPath $iconOutput).Length) bytes)"
