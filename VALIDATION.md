# Version 0.1.0 validation

Checked on Windows x64 with MSVC 19.51 and two displays: 3840 x 2160 at
(0, 0), and 2560 x 1600 at (3840, 537).

## Build and package

- `./build.ps1 -Test -Publish -Version 0.1.0`: passed.
- `./scripts/package.ps1 -ExpectedVersion 0.1.0`: passed. One native x64
  executable, matching product version, no CLR header or external C++ runtime.
- `./tests/package-tests.ps1`: passed. Rejects extra or missing files,
  directories, version mismatches, nested archive output, invalid executables,
  x86 images, managed images, and invalid version fields.
- Release executable: 125,440 bytes. Local ZIP: 63,475 bytes. CI toolchain
  versions can produce different sizes and hashes.
- Version-update skill validator and workflow YAML parse: passed.

## Desktop and memory checks

`./scripts/smoke-test.ps1` passed on the published Release executable:

- Real mouse drags in all four directions produced exact 100 x 80 images.
  Each pixel matched the expected two-color pattern, including all edges.
- Image pixels remained on the clipboard after Pluck exited.
- Escape during a drag, right-click, and an empty selection preserved the
  clipboard. An empty selection kept rectangle mode open.
- A short clipboard lock was retried successfully. A lock beyond the retry
  limit returned to selection and permitted a new capture.
- Each connected display and a selection across their shared edge produced
  exact physical pixel crops.
- The saved selection screenshot was visually inspected. The live desktop
  was dimmed outside the rectangle, with a visible border and cancel hint.

`./build.ps1 -Configuration RelWithDebInfo -Sanitize -Test` passed. The complete
desktop test also passed with the AddressSanitizer application and test binaries.
No AddressSanitizer error was reported. Core tests also check negative-coordinate
geometry, one-pixel areas, DIB row order and padding, invalid crops, and GDI
resource lifetime.

## Startup

Ten Release launches measured **85.2 ms median**, 78.6 ms minimum, and 156.0 ms
maximum from process creation to a painted foreground overlay and compositor
synchronization. These are local samples with the Windows file cache retained,
not cold-boot measurements or a timing guarantee. Diagnostic builds are slower.

Capture occurs after selection. The startup path does not capture the full
desktop or initialize a video pipeline.

## Limits of these checks

The desktop tests used the current two-display layout. Negative display origins
were covered by geometry tests, not a physical monitor rearrangement. HDR color
accuracy, protected content, and secure desktops are outside this release's
capture guarantees. CI runs the core, package, and sanitizer checks; real desktop
interaction requires an unlocked local session.
