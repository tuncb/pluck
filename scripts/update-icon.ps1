#requires -Version 7.0
# Generate the Windows icon from the SVG. Normal builds use the ICO directly.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$iconSource = Join-Path $projectRoot 'assets/Pluck.svg'
$iconOutput = Join-Path $projectRoot 'assets/Pluck.ico'
$imageMagick = Get-Command magick -ErrorAction SilentlyContinue
if (-not $imageMagick) { throw 'Install ImageMagick with SVG support to generate the app icon.' }

# Render at a larger size. Keep 32-bit alpha at each Windows icon size.
& $imageMagick.Source -background none -density 1536 $iconSource -resize 256x256 -depth 8 -type TrueColorAlpha -define icon:auto-resize=256,128,96,64,48,40,32,24,20,16 $iconOutput
if ($LASTEXITCODE -ne 0) { throw 'App icon generation failed.' }
Write-Output "Windows icon: $iconOutput"
