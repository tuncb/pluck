# Pluck

A small Windows screenshot app. A companion to [Yoink](https://github.com/tuncb/yoink).

1. Open **Pluck.exe**.
2. Drag a rectangle.
3. Release the mouse button. The image is on the clipboard and Pluck exits.

Paste the image into an app that accepts images. Press **Escape** or **right-click**
to cancel. A click without a drag does not copy an image. There is no launcher,
editor, tray process, installer, settings file, or update check.

## Download

Download **[pluck-windows-x64.zip](https://github.com/tuncb/pluck/releases/latest/download/pluck-windows-x64.zip)**
from [GitHub Releases](https://github.com/tuncb/pluck/releases). Extract the ZIP and
run **Pluck.exe**. The package contains only this executable. It needs no .NET or
separate Visual C++ runtime installation.

The target is Windows 10 version 1703 or later, or Windows 11, on x64.
The desktop stays live while you select. Selection can span displays, including
displays with negative coordinates. Pixels are not resized. The mouse pointer,
selection border, and dimming are not in the copied image.

Capture uses the Windows GDI desktop image. Output is an SDR bitmap; HDR color
accuracy is not guaranteed. The image is captured when you release the mouse.
Protected content and secure desktops cannot be
captured. A display-layout change or loss of foreground focus cancels selection.
If another app holds the clipboard, Pluck retries briefly. If copying still
fails, drag again or cancel.

## Build and test

Use PowerShell 7, Visual Studio 2022 or later with **Desktop development with
C++**, a Windows SDK, CMake 3.25 or later, and Ninja. The Visual Studio **C++ CMake
tools** component can supply CMake and Ninja.

```powershell
./scripts/bootstrap.ps1
./build.ps1 -Test -Publish
./scripts/package.ps1 -ExpectedVersion 0.2.1
./tests/package-tests.ps1
./run.ps1
```

Build output is in **build/Release/bin**. The portable executable is in
**artifacts/win-x64**. The ZIP and SHA-256 file are in **artifacts/packages**.
Use `-Jobs 4` to set build concurrency or `-Clean` for a full rebuild.

Run these checks on an unlocked Windows desktop:

```powershell
./scripts/smoke-test.ps1
./build.ps1 -Configuration RelWithDebInfo -Sanitize -Test
```

The desktop test briefly uses the mouse and keyboard. It saves and restores
clipboard formats that Windows can duplicate. It checks real drag input, exact
crop pixels, cancellation, clipboard contention, and image availability after
exit. It records startup times and a selection screenshot under **artifacts/smoke**.
Core and package tests run in CI; desktop tests need a local interactive session.

## Design and releases

Pluck follows Yoink's C++20, Win32, CMake, static MSVC runtime, and PowerShell
conventions. A native translucent overlay opens before any screenshot is taken.
After selection, the overlay is hidden, the desktop compositor is synchronized,
and GDI captures only the selected rectangle. There is no video pipeline, COM
initialization, dependency extraction, or disk/network work on the normal startup
path. The selector uses physical pixel coordinates
and a Per-Monitor V2 DPI manifest. The clipboard receives a complete 24-bit
`CF_DIB`, so the image remains available after the process exits.

`VERSION` is the version source for CMake, executable resources, and packaging.
Pushes and pull requests run release builds, core tests, package failure checks,
and AddressSanitizer tests. A matching `vX.Y.Z` tag runs those checks again and
publishes the ZIP and checksum in **this repository**, using the matching
`changelog.txt` section as release notes.

Use [.agents/skills/update-version/SKILL.md](.agents/skills/update-version/SKILL.md)
for version updates and releases.
