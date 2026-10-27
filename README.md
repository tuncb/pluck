# Pluck

A small Windows screenshot app. A companion to [Yoink](https://github.com/tuncb/yoink).

1. Open **Pluck.exe**.
2. Drag a rectangle.
3. Release the mouse button. The image is on the clipboard and Pluck exits.

Paste the image into an app that accepts images. Press **Escape** or **right-click**
to cancel. A click without a drag does not copy an image.

Press **F2** before or during selection to open **Pluck Settings**. This cancels
selection and preserves the clipboard. Settings runs in a separate process.
The small F2 hint appears at the bottom of the display under the pointer. It hides
when you press the mouse button and is never part of the copied image.

## Settings, installation, and updates

You can also run **Pluck.exe --settings**. The native window follows the Windows
light or dark theme and uses the mint and teal colors from the Pluck icon.

Select **Install Pluck** to install for the current Windows user in
**%LOCALAPPDATA%/Programs/Pluck**. Pluck adds two Start menu shortcuts:
**Pluck** starts capture; **Pluck Settings** opens settings. The original portable
copy is retained. Installation does not require administrator rights.

Select **Check for updates** to check the latest stable GitHub release. Checks
run only on request. From the installed copy, select **Update** when a newer
release is available. Pluck downloads the executable, checks its SHA-256 and
version, then starts a temporary helper. The helper waits for Settings and any
active capture to exit before it replaces the app. It reopens Settings after
replacement. New captures are blocked only while the helper completes this work.
Older releases without executable update assets require a manual GitHub download.

Use **Restore shortcuts** to recreate the Start menu shortcuts. Select **Uninstall**
or use Windows **Installed apps** to remove the installed executable and shortcuts.
Unknown files in the installation folder are retained. Completed helper files are
removed on a later settings launch. An incomplete replacement retains its backup
as **Pluck.previous.exe**; restore that file to **Pluck.exe** before retrying.

Normal launch goes directly to capture. There is no resident tray process,
startup registration, automatic update check, or capture configuration file.

## Download

Download **[pluck-windows-x64.zip](https://github.com/tuncb/pluck/releases/latest/download/pluck-windows-x64.zip)**
from [GitHub Releases](https://github.com/tuncb/pluck/releases). Extract the ZIP and
run **Pluck.exe**. The package contains only this executable. It needs no .NET or
separate Visual C++ runtime installation.

The target is Windows 10 version 1703 or later, or Windows 11, on x64.
Pluck saves an image of the desktop when it opens. This keeps open menus in the
image while you select. Selection can span displays, including
displays with negative coordinates. Pixels are not resized. The mouse pointer,
selection border, and dimming are not in the copied image.

Capture uses the Windows GDI desktop image. Output is an SDR bitmap; HDR color
accuracy is not guaranteed. The image is captured before selection starts.
Protected content and secure desktops cannot be
captured. A display-layout change or loss of foreground focus cancels selection.
If another app holds the clipboard, Pluck retries briefly. If copying still
fails, drag again on the same saved image or cancel.

## Build and test

Use PowerShell 7, Visual Studio 2022 or later with **Desktop development with
C++**, a Windows SDK, CMake 3.25 or later, and Ninja. The Visual Studio **C++ CMake
tools** component can supply CMake and Ninja.

```powershell
./scripts/bootstrap.ps1
./build.ps1 -Test -Publish
./scripts/package.ps1
./tests/package-tests.ps1
./run.ps1
```

Build output is in **build/Release/bin**. The portable executable is in
**artifacts/win-x64**. The ZIP, executable update asset, and SHA-256 files are in
**artifacts/packages**.
Use `-Jobs 4` to set build concurrency or `-Clean` for a full rebuild.

Run these checks on an unlocked Windows desktop:

```powershell
./scripts/smoke-test.ps1
./build.ps1 -Configuration RelWithDebInfo -Sanitize -Test
```

The desktop test briefly uses the mouse and keyboard. It saves and restores
clipboard formats that Windows can duplicate. It checks real drag input, exact
crop pixels, open popup menus, F2 access, hint visibility, cancellation, clipboard contention, and image
availability after exit. It records startup times and selection screenshots under
**artifacts/smoke**.
Core, management, and package tests run in CI; desktop tests need a local interactive session.

## Design and releases

Pluck follows Yoink's C++20, Win32, CMake, static MSVC runtime, and PowerShell
conventions. GDI saves the desktop before the selection window opens or takes
focus. The window shows this saved image, dimmed outside the selected rectangle.
After selection, Pluck copies the selected pixels from the saved image. There is
no video pipeline, COM initialization, dependency extraction, or disk/network
work on the normal startup path. The selector uses physical pixel coordinates
and a Per-Monitor V2 DPI manifest. The clipboard receives a complete 24-bit
`CF_DIB`, so the image remains available after the process exits.

The icon source is **assets/Pluck.svg**. Run **scripts/update-icon.ps1** to generate
**assets/Pluck.ico** after an icon change. This script needs ImageMagick with SVG
support. Normal builds use the ICO file and do not need ImageMagick.

`VERSION` is the version source for CMake, executable resources, and packaging.
Pushes and pull requests run release builds, core tests, package failure checks,
and AddressSanitizer tests. A matching `vX.Y.Z` tag runs those checks again and
publishes the ZIP, executable, and checksums in **this repository**, using the matching
`changelog.txt` section as release notes.

Use [.agents/skills/update-version/SKILL.md](.agents/skills/update-version/SKILL.md)
for version updates and releases.
