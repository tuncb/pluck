---
name: update-version
description: Update the Pluck version and prepare or publish a release when the user asks to bump the version or release Pluck.
---

# Version update workflow

- Source code and releases both use the public `tuncb/pluck` repository. Keep `origin` at `https://github.com/tuncb/pluck.git`. The release workflow uses its own `GITHUB_TOKEN` with `contents: write`; no separate release repository or `RELEASES_TOKEN` is needed.
- Read `VERSION`, `changelog.txt`, and changes since the previous release tag. Set `VERSION` to the requested `X.Y.Z`. Each field must be at most 65535. If no version was specified, use the project's next patch version unless the changes require a minor or major version; state the choice.
- Put notable user changes under the matching `vX.Y.Z` heading in `changelog.txt`, newest release first. Move relevant `Unreleased` entries. Omit agent instructions, experiments, and investigations. For an initial release, describe the implemented behavior.
- In PowerShell 7, run `./build.ps1 -Test -Publish -Version X.Y.Z`, `./scripts/package.ps1 -ExpectedVersion X.Y.Z`, and `./tests/package-tests.ps1`. For capture or selection changes, also run `./scripts/smoke-test.ps1` on an unlocked desktop. Check the results before committing.
- Confirm the package contains only `Pluck.exe`, has the expected embedded version, and is native Windows x64 with a static runtime. Packaging performs these checks and verifies the ZIP bytes. Release assets are `pluck-windows-x64.zip` and `pluck-windows-x64.zip.sha256`.
- Review the Git diff. Commit the intended release changes, including `VERSION` and `changelog.txt`. Preserve unrelated user changes unless the user asked to include them. Preparation alone does not authorize publication; proceed with the following steps when publishing is part of the user's request or existing authorization.
- After the commit succeeds, create the annotated tag `vX.Y.Z` at that commit. Push the branch and only that release tag to `origin` (`git push origin main`, then `git push origin vX.Y.Z`). Do not push every local tag. Do not move an existing published tag or replace published binaries; use a new version for corrections.
- `.github/workflows/release.yml` validates the tag against `VERSION` and the changelog, calls the reusable build/test workflow, then publishes in the same repository. It creates a draft with both verified assets before publication. A failed run can resume a draft; it refuses to overwrite an already published release.
- Wait for the `Release` run for the exact tag and commit to succeed. Confirm the public release is stable, is not a draft, has the matching notes, and contains both assets. Download the ZIP and checksum, compare SHA-256, and verify the extracted executable's version. Report a failed run as a failure; a tag push alone is not a completed release.
- Pluck has no updater, installer, FFmpeg bundle, or separate source archive. Do not add Yoink's updater checks or private/public repository split to this workflow.
