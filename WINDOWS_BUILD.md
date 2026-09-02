# Windows x64 build for release-v0.6

The workflow in `.github/workflows/windows.yml` builds the `release-v0.6`
branch as a portable Windows x64 application using MSYS2, MinGW-w64 and Qt 5.

## Run the build

1. Open the fork's **Actions** tab.
2. Select **Build Windows x64 (release-v0.6)**.
3. Choose the `release-v0.6` branch and run the workflow.
4. Download **HDRMerge-release-v0.6-Windows-x64** from the run's
   **Artifacts** section.

The artifact contains `hdrmerge.exe`, `hdrmerge-nogui.exe`, the Qt platform
plugin, and the runtime DLL dependencies.

## Notes

- Builds run after relevant pushes to `release-v0.6` and can also be started
  manually.
- Artifacts expire after seven days.
- ALGLIB 3.15.0 GPL is downloaded from its official site during the build.
- The workflow uses a standard `windows-latest` runner and does not request a
  paid larger runner.
